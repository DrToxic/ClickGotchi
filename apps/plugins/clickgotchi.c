/***************************************************************************
 *
 * ClickGotchi - Stage 1: the pet (RockBox plugin, iPod Video 5.5G first)
 *
 * A port of the CYDgotchi / LoRagotchi pet. Sprites, scenery and the pet
 * rules come from the LoRagotchi-DX repository (branch openclaw/cyd-pet-share):
 *   - sprite masks (32x24, 2 frames, 2 bpp)  clickgotchi_art.h, generated from
 *       examples/companion_radio/ui-new/LoRaFish{Water,Air,Land}Art.h
 *   - raster + colours                       LoRaFishSpriteRaster.h,
 *       LoRaFishSpriteColours.h, LoRaFishColour.h (drawn at the CYD's scale 3)
 *   - scenery                                LoRaFishScenery.h (Water, Air, Land)
 *   - egg / hatch / memorial art             LoRaFishLifeArt.h
 *   - care rules                             LoRaFishHappiness.h, LoRaFishCare.h,
 *       CydCare.h (meal once an hour, waste, clean, 7-day life, life stages)
 *   - species names / life-stage names       LoRaFishSpecies.h
 *
 * What is NOT here (decisions, 2026-11-25):
 *   - no breeding with visitors, no transfer (BLE / WiFi / LoRa)
 *   - no automatic fertilisation: "Fertilise" is a menu option the owner picks
 *   - the plain fish (species 0) and the naturalidle state machine are left out;
 *     the pet wanders with a small, simpler motion model
 *
 * What the pet reacts to:
 *   - clock (RTC): asleep between the Sleep time and Wake time (Options; equal times = never asleep),
 *     no decay while asleep
 *   - charger: the pet rests while the iPod charges (no decay), if Rest while charging is Yes (default)
 *   - battery: at 20% or less (not charging) the pet is tired and stays put
 *   - USB: plugging in USB saves and quits, as in the baseline
 *
 * Stage 2 (menu, create a pet, reset):
 *   - Options is a RockBox list screen (up/down, SELECT changes or opens, CANCEL goes back),
 *     saved in clickgotchi.cfg. It holds the settings, Change pet name and Reset pet.
 *   - With no pet the plugin opens "Adopt a pet" > "New pet": the CYDgotchi generator
 *     (CydPetGenerator.h, itself the radio's CHOOSE PET): Species (Water / Air / Land /
 *     Mystery egg, then the animal or a Mystery egg), Name, Mystery colours, Choose colours
 *     (12-colour picker), Start pet > "Start this pet?" preview > create. A new pet is an egg.
 *   - Reset pet (Options): the radio's PetResetConfirmation ("Start a new egg?", Cancel by
 *     default, select "Yes, reset pet", then confirm) with the CYD's 2-second hold. Then
 *     "Adopt a pet" opens again.
 *
 * Controls (standard RockBox actions):
 *   MENU    care menu (Feed, Clean, Fertilise, Info, Options)
 *   CANCEL  quit
 *
 * Layout: on a colour screen of 240x160 or more the pet is drawn at the CYD's
 * size (96x72). Other screens keep the compact 6-row text layout of the
 * baseline (it fits 128x64), with the pet summarised in text.
 *
 * This file is GPL-2.0-or-later, like the RockBox plugins it sits next to.
 *
 ****************************************************************************/
#include "plugin.h"

#include "lib/configfile.h"
#include "clickgotchi_art.h"

#define CONFIG_FILE_NAME "clickgotchi.cfg"
#define STATE_FILE       PLUGIN_GAMES_DATA_DIR "/clickgotchi.state"
#define PET_FILE         PLUGIN_GAMES_DATA_DIR "/clickgotchi.pet"

#define MS_PER_TICK (1000 / HZ)
#define NAME_MAX_CHARS 16        /* LoRaFishIdentity::kNameMax */

/* ------------------------------------------------------------------------
 * Settings: saved in clickgotchi.cfg, shown in the RockBox option screen
 * ---------------------------------------------------------------------- */
static bool show_seconds = true;
static bool keep_backlight_on_usb = true;
/* Sleep and Wake, whole hours 0-23, the radio's Schedule rule: the pet sleeps from sleep_hour to
 * wake_hour (wrapping past midnight), and sleep_hour == wake_hour means never asleep. */
static int sleep_hour = 22;
static int wake_hour = 7;
static bool rest_while_charging = true; /* pet rests while the iPod charges (optional, owner 2026-11-25) */
/* Speed, as the radio's Rendering setting: 0 Smooth (~30 fps), 1 Simulation (10 fps), 2 Efficient 1 FPS */
static int render_speed = 0;
static uint32_t clean_hold_ms = 0;   /* last time SELECT-hold cleaned (anti-repeat) */
static bool music_reactive = false;  /* music keeps the pet awake and shows a stereo level meter (owner 2026-11-25) */

static struct configdata config[] = {
    {TYPE_BOOL, 0, 1, { .bool_p = &show_seconds },           "show_seconds",           NULL},
    {TYPE_BOOL, 0, 1, { .bool_p = &keep_backlight_on_usb },  "keep_backlight_on_usb",  NULL},
    {TYPE_INT,  0, 23, { .int_p  = &sleep_hour },            "sleep_hour",             NULL},
    {TYPE_INT,  0, 23, { .int_p  = &wake_hour },             "wake_hour",              NULL},
    {TYPE_BOOL, 0, 1, { .bool_p = &rest_while_charging },    "rest_while_charging",    NULL},
    {TYPE_INT,  0, 2, { .int_p  = &render_speed },           "render_speed",           NULL},
    {TYPE_BOOL, 0, 1, { .bool_p = &music_reactive },         "music_reactive",         NULL},
};
#define CONFIG_COUNT ((int)(sizeof(config) / sizeof(config[0])))

/* Saved state: how many times the plugin has been started */
static long boot_count = 0;

static void state_load(void)
{
    char buf[32];
    int fd = rb->open(STATE_FILE, O_RDONLY);
    if (fd < 0)
        return;
    ssize_t n = rb->read(fd, buf, sizeof(buf) - 1);
    rb->close(fd);
    if (n > 0) {
        buf[n] = '\0';
        boot_count = rb->atoi(buf);
    }
}

static void state_save(void)
{
    char buf[32];
    int len = rb->snprintf(buf, sizeof(buf), "%ld\n", boot_count);
    int fd = rb->open(STATE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    rb->write(fd, buf, len);
    rb->close(fd);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(*rb->current_tick) * MS_PER_TICK;
}

/* ------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */
static uint32_t rng_state = 1;

static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* uniform in lo..hi */
static uint32_t rng_range(uint32_t lo, uint32_t hi)
{
    return lo + rng() % (hi - lo + 1u);
}

/* LoRaFishLifeArt::mix */
static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dUL; x ^= x >> 15;
    x *= 0x846ca68bUL; return x ^ (x >> 16);
}

/* sine in thousandths for a 64-step circle (integer only: plugins have no libm) */
static int sin64(int phase)
{
    static const short q[17] = {0, 98, 195, 290, 383, 471, 556, 634, 707,
                                773, 831, 882, 924, 957, 981, 995, 1000};
    int p = phase & 63, quad = p >> 4, i = p & 15;
    switch (quad) {
    case 0:  return q[i];
    case 1:  return q[16 - i];
    case 2:  return -q[i];
    default: return -q[16 - i];
    }
}

/* ------------------------------------------------------------------------
 * Species registry (LoRaFishSpecies.h): Water 1..14, Air 256..273, Land 512..555.
 * The plain Tropical fish (0) is the first Water animal, as on the radio (mask from the CYD fish bitmap).
 * ---------------------------------------------------------------------- */
#define SP_WATER_COUNT 15
#define SP_AIR_COUNT   18
#define SP_LAND_COUNT  44
#define SP_TOTAL       (SP_WATER_COUNT + SP_AIR_COUNT + SP_LAND_COUNT)
#define ID_AIR0        256
#define ID_LAND0       512

enum {
    ID_SHARK = 2, ID_DOLPHIN = 3, ID_WHALE = 4, ID_SEAL = 5, ID_CRAB = 9, ID_JELLYFISH = 11,
    ID_SHELLDWELLER = 12,
    ID_PENGUIN = 261, ID_FLAMINGO = 263, ID_PEACOCK = 264, ID_TURKEY = 265, ID_ROOSTER = 266,
    ID_HONEYBEE = 269, ID_BUTTERFLY = 270, ID_FLY = 271, ID_WORM = 272, ID_BUG = 273,
    ID_FROG = 540, ID_CROCODILE = 541, ID_LIZARD = 542, ID_TURTLE = 543, ID_SNAKE = 544,
    ID_DRAGON = 546, ID_GRIFFIN = 547, ID_PHOENIX = 548
};

static bool sp_water(unsigned id) { return id < SP_WATER_COUNT; }
static bool sp_air(unsigned id)   { return id >= ID_AIR0 && id < ID_AIR0 + SP_AIR_COUNT; }
static bool sp_land(unsigned id)  { return id >= ID_LAND0 && id < ID_LAND0 + SP_LAND_COUNT; }
static bool sp_known(unsigned id) { return sp_water(id) || sp_air(id) || sp_land(id); }

/* index 0..SP_TOTAL-1 in registry order -> species id (LoRaFishSpecies::anyMystery order) */
static uint16_t sp_from_index(unsigned i)
{
    if (i < SP_WATER_COUNT) return (uint16_t)i;
    i -= SP_WATER_COUNT;
    if (i < SP_AIR_COUNT) return (uint16_t)(ID_AIR0 + i);
    i -= SP_AIR_COUNT;
    return (uint16_t)(ID_LAND0 + (i < SP_LAND_COUNT ? i : 0));
}

static const char *const water_names[SP_WATER_COUNT] = {
    "Tropical fish", "Blowfish", "Shark", "Dolphin", "Whale", "Seal", "Shrimp", "Squid", "Lobster",
    "Crab", "Octopus", "Jellyfish", "Shell-dweller", "Sea star", "Anglerfish"
};
static const char *const air_names[SP_AIR_COUNT] = {
    "Eagle", "Bird", "Owl", "Duck", "Swan", "Penguin", "Parrot", "Flamingo", "Peacock",
    "Turkey", "Rooster", "Dove", "Bat", "Honeybee", "Butterfly", "Fly", "Worm", "Bug"
};
static const char *const land_names[SP_LAND_COUNT] = {
    "Dog", "Cat", "Horse", "Cow", "Elephant", "Pig", "Ram", "Sheep", "Goat", "Giraffe",
    "Kangaroo", "Llama", "Deer", "Mouse", "Hamster", "Rabbit", "Hedgehog", "Koala", "Panda",
    "Sloth", "Otter", "Skunk", "Badger", "Hippo", "Rhino", "Camel", "Bactrian camel", "Bison",
    "Frog", "Crocodile", "Lizard", "Turtle", "Snake", "Unicorn", "Dragon", "Griffin", "Phoenix",
    "Sauropod", "T-Rex", "Wolf", "Fox", "Bear", "Kitsune", "Red Panda"
};

static const char *sp_label(unsigned id)
{
    if (sp_water(id)) return water_names[id];
    if (sp_air(id))   return air_names[id - ID_AIR0];
    if (sp_land(id))  return land_names[id - ID_LAND0];
    return "Unknown";
}

/* LoRaFishSpecies::lifeStage (stage 0..3 -> species-aware name) */
static const char *sp_stage_name(unsigned id, int stage)
{
    if (stage >= 3) return "Elder";
    if (stage == 2) return "Adult";
    if (stage == 1) {
        if (id == ID_BUTTERFLY) return "Chrysalis";
        if (id == ID_HONEYBEE || id == ID_FLY) return "Pupa";
        if (id == ID_FROG) return "Froglet";
        return "Juvenile";
    }
    if (sp_land(id)) {
        switch (id) {
        case 512: case 532: case 525: case 526: case 551: return "Pup";           /* Dog Otter Mouse Hamster Wolf */
        case 513: case 527: case 533: case 552: case 554: return "Kit";           /* Cat Rabbit Skunk Fox Kitsune */
        case 514: case 545: return "Foal";
        case 517: return "Piglet";
        case 518: case 519: return "Lamb";
        case 520: return "Kid";
        case 522: case 529: return "Joey";
        case 523: return "Cria";
        case 524: return "Fawn";
        case 528: return "Hoglet";
        case 530: case 531: case 534: case 553: case 555: return "Cub";           /* Panda Sloth Badger Bear RedPanda */
        case ID_FROG: return "Tadpole";
        case 541: case 542: case 543: case 544: case 546: case 547: case 548: case 549: case 550:
            return "Hatchling";
        default: return "Calf";
        }
    }
    if (!sp_air(id)) return "Fry";
    switch (id) {
    case 259: return "Duckling";
    case 256: return "Eaglet";
    case 260: return "Cygnet";
    case 258: return "Owlet";
    case ID_BUTTERFLY: return "Caterpillar";
    case ID_HONEYBEE: case ID_FLY: case ID_BUG: return "Larva";
    case 268: return "Pup";
    case ID_WORM: return "Hatchling";
    case ID_PENGUIN: case 257: case 262: case ID_FLAMINGO: case ID_PEACOCK: case ID_TURKEY:
    case ID_ROOSTER: case 267:
        return "Chick";
    default: return "Hatchling";
    }
}

enum movement { MV_SWIM, MV_FLY, MV_FLUTTER, MV_WALK, MV_CRAWL };

static enum movement sp_movement(unsigned id)
{
    if (sp_land(id)) {
        if (id == ID_FROG || id == ID_CROCODILE || id == ID_LIZARD || id == ID_TURTLE || id == ID_SNAKE)
            return MV_CRAWL;
        if (id == ID_DRAGON || id == ID_GRIFFIN || id == ID_PHOENIX)
            return MV_FLY;
        return MV_WALK;
    }
    if (!sp_air(id)) return MV_SWIM;
    if (id == ID_WORM || id == ID_BUG) return MV_CRAWL;
    if (id == ID_PENGUIN || id == ID_FLAMINGO || id == ID_PEACOCK || id == ID_TURKEY || id == ID_ROOSTER)
        return MV_WALK;
    if (id == ID_HONEYBEE || id == ID_BUTTERFLY || id == ID_FLY) return MV_FLUTTER;
    return MV_FLY;
}

/* the mask of one frame, or NULL */
static const unsigned char *sp_mask(unsigned id, unsigned frame)
{
    frame &= 1u;
    if (sp_land(id)) return cg_land_masks[id - ID_LAND0][frame];
    if (sp_air(id))  return cg_air_masks[id - ID_AIR0][frame];
    if (sp_water(id)) return cg_water_masks[id][frame];
    return NULL;
}

/* ------------------------------------------------------------------------
 * Colours (LoRaFishColour.h, LoRaFishSpriteColours.h). All RGB565, as in the source.
 * ---------------------------------------------------------------------- */
#define GENE_MONO  6
#define GENE_TINT  7

static bool gene_valid(unsigned v) { return v <= 6 || (v >= 8 && v <= 247); }

static unsigned gene_hue(unsigned v)
{
    static const unsigned char p[6] = {235, 20, 40, 80, 120, 200};
    return v < 6 ? p[v] : (v >= 8 && v <= 247) ? v - 8 : 0;
}

static uint16_t gene_rgb565(unsigned v)
{
    static const uint16_t pal[6] = {0xF986, 0xFC00, 0xFFE0, 0x07E0, 0x07FF, 0xF81F};
    unsigned h, sector, ramp, r = 0, g = 0, b = 0;
    if (v < 6) return pal[v];
    h = gene_hue(v); sector = h / 40; ramp = (h % 40) * 255 / 40;
    switch (sector) {
    case 0: r = 255; g = ramp; break;
    case 1: r = 255 - ramp; g = 255; break;
    case 2: g = 255; b = ramp; break;
    case 3: g = 255 - ramp; b = 255; break;
    case 4: r = ramp; b = 255; break;
    default: r = 255; b = 255 - ramp; break;
    }
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* LoRaFishSpecies::mysteryColour: role 0 accent, 1 body, 2 eyes */
static uint8_t mystery_colour(uint32_t seed, unsigned role)
{
    return (uint8_t)(8 + mix32(seed + 0x9e3779b9UL * (role + 1)) % 240);
}

/* colour of a gene; Mono and anything invalid take white (the tank's foreground) */
static uint16_t gene_colour(unsigned gene)
{
    return gene_valid(gene) && gene != GENE_MONO ? gene_rgb565(gene) : 0xFFFF;
}

#define IVORY 0xFFBC
static uint16_t tint565(uint16_t c, unsigned weight)
{
    unsigned r = (((c >> 11) & 31) * weight + ((IVORY >> 11) & 31) * (8 - weight) + 4) / 8;
    unsigned g = (((c >> 5) & 63) * weight + ((IVORY >> 5) & 63) * (8 - weight) + 4) / 8;
    unsigned b = ((c & 31) * weight + (IVORY & 31) * (8 - weight) + 4) / 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t juvenile_tint(unsigned gene, uint16_t c, unsigned weight)
{
    return !gene_valid(gene) || gene == GENE_MONO ? c : tint565(c, weight);
}

/* the ink that shows on a body colour: dark on light, light on dark */
static uint16_t ink_for(uint16_t body)
{
    unsigned light = ((body >> 11) & 31) * 299 * 2 + ((body >> 5) & 63) * 587 + (body & 31) * 114 * 2;
    return light >= 31500 ? 0x2104 : 0xDEFB;
}

struct colours { uint16_t body, fins, eyes; };

/* CydTankArt::coloursFor (the colour display case of LoRaFishSpriteColours::forStage) */
static struct colours colours_for(unsigned body, unsigned accent, unsigned eyes, int stage)
{
    struct colours c;
    c.body = gene_colour(body); c.fins = gene_colour(accent); c.eyes = gene_colour(eyes);
    if (stage < 2) {
        uint16_t bc = c.body, fc = c.fins;
        c.body = juvenile_tint(body, bc, stage == 0 ? 2 : 5);
        c.fins = stage == 0 ? juvenile_tint(body, bc, 5) : juvenile_tint(accent, fc, 6);
    }
    if (stage < 2 || c.eyes == c.body || eyes == GENE_MONO || eyes == GENE_TINT)
        c.eyes = ink_for(c.body);
    return c;
}

/* ------------------------------------------------------------------------
 * Care rules (LoRaFishCare.h, LoRaFishHappiness.h, CydCare.h)
 * ---------------------------------------------------------------------- */
#define EGG_MINUTES        5UL
#define LIFESPAN_MINUTES   (7UL * 24UL * 60UL)
#define MAX_WASTE          3
#define WASTE_MIN_MS       30000UL
#define WASTE_SPREAD_MS    60001UL
#define CLEAN_BONUS        3
#define START_HAPPINESS    80
#define DECAY_MS           (30UL * 60UL * 1000UL)
#define MEAL_MS            (60UL * 60UL * 1000UL)
#define HATCH_MS           12000UL
#define HATCH_PHASE_MS     2400UL
#define SUCCESSION_MINUTES 5UL    /* memorial wait before the next generation */

static int stage_for_age(uint32_t mins)
{
    if (mins < 60) return 0;
    if (mins < 6UL * 60UL) return 1;
    if (mins < 3UL * 24UL * 60UL) return 2;
    return 3;
}

struct pet {
    bool     exists;
    uint16_t species;
    uint8_t  body, accent, eyes;
    char     name[NAME_MAX_CHARS + 1];   /* optional, "" = none (LoRaFishIdentity) */
    uint16_t generation;
    uint32_t seed;
    bool     mystery;        /* a fresh Mystery egg: question mark until it hatches */
    uint32_t age_min;
    uint8_t  happy;
    uint8_t  waste;
    bool     has_egg;        /* offspring egg laid by the manual Fertilise option */
    uint32_t awake_ms;       /* progress to the next 30-minute happiness decay */
    uint32_t meal_cd_ms;
    uint32_t age_acc_ms;
    uint32_t waste_in_ms;
};

static struct pet pet;
static bool pet_dirty = false;
static uint32_t pet_last_save = 0;
static uint32_t age_since_save = 0;

static bool pet_is_egg(void)      { return pet.exists && pet.age_min < EGG_MINUTES; }
static bool pet_is_memorial(void) { return pet.exists && pet.age_min >= LIFESPAN_MINUTES; }
static bool pet_living(void)
{
    return pet.exists && pet.age_min >= EGG_MINUTES && pet.age_min < LIFESPAN_MINUTES;
}

/* ------------------------------------------------------------------------
 * New-pet generator: the CYDgotchi's CydGen::Generator (CydPetGenerator.h), which is the radio's
 * CHOOSE PET setup. Same choices in the same order; same one-seed rule for Mystery eggs and the
 * "every later press is a fresh set" rule for Mystery colours. The plain fish (id 0) has no art
 * here (Stage 1 decision), so the Water set lists 14 animals.
 * ---------------------------------------------------------------------- */
#define SET_COUNT 3                 /* 0 Water, 1 Air, 2 Land (LoRaFishSpecies::setCount) */
#define SET_ANY   SET_COUNT         /* the set picker's extra "Mystery egg" row */
enum { GC_ACCENT = 0, GC_BODY = 1, GC_EYES = 2, GC_COUNT = 3 };

static unsigned set_size(unsigned set)
{
    return set == 2 ? SP_LAND_COUNT : set == 1 ? SP_AIR_COUNT : SP_WATER_COUNT;
}
static unsigned set_base(unsigned set) { return set == 2 ? ID_LAND0 : set == 1 ? ID_AIR0 : 0; }
static const char *set_label(unsigned set) { return set == 2 ? "Land" : set == 1 ? "Air" : "Water"; }
static unsigned set_of(unsigned id) { return sp_land(id) ? 2 : sp_air(id) ? 1 : 0; }
static uint16_t set_selection_id(unsigned set, unsigned index)
{
    return (uint16_t)(set_base(set) + (index < set_size(set) ? index : 0));
}
/* LoRaFishSpecies::setMystery / anyMystery */
static uint16_t set_mystery(uint32_t seed, unsigned set)
{
    return set_selection_id(set, mix32(seed) % set_size(set));
}
static uint16_t any_mystery(uint32_t seed) { return sp_from_index(mix32(seed) % SP_TOTAL); }

/* The colour picker (LoRaFishColour::kPicker): six palette colours, Mono, five hand-picked hues */
#define PICKER_COUNT 12
static const uint8_t picker[PICKER_COUNT] = {0, 1, 2, 3, 4, 5, 6, 68, 148, 168, 188, 228};
static const char *const picker_names[PICKER_COUNT] = {
    "Coral", "Tangerine", "Gold", "Mint", "Lagoon", "Orchid", "Mono",
    "Lime", "Azure", "Blue", "Violet", "Rose"
};

/* LoRaFishColour::pickerIndex: exact, else the nearest hue */
static unsigned picker_index(unsigned gene)
{
    unsigned i, best = 241, sel = 0;
    for (i = 0; i < PICKER_COUNT; i++)
        if (picker[i] == gene) return i;
    for (i = 0; i < PICKER_COUNT; i++) {
        int d = (int)gene_hue(gene) - (int)gene_hue(picker[i]);
        if (d < 0) d = -d;
        if (d > 120) d = 240 - d;
        if ((unsigned)d < best) { best = (unsigned)d; sel = i; }
    }
    return sel;
}

/* CydGen::colourName: a picker colour's name; any other hue is a mystery colour */
static const char *colour_name(unsigned gene)
{
    unsigned i;
    for (i = 0; i < PICKER_COUNT; i++)
        if (picker[i] == gene) return picker_names[i];
    return "Mystery";
}

/* CydGen::cycleGene: the screen-only hue a mystery colour shows (never its real gene) */
static uint8_t cycle_gene(unsigned square, uint32_t since_ms)
{
    return (uint8_t)(8 + ((square % 3) * 80 + (since_ms / 300UL) * 20) % 240);
}

struct gen {
    char     name[NAME_MAX_CHARS + 1];
    uint16_t species;
    uint8_t  set;                 /* 0..2, or SET_ANY for a Mystery egg from any set */
    bool     mystery;             /* "Mystery egg": the animal is not shown until it is born */
    bool     rolled;
    bool     part_mystery[GC_COUNT];
    uint32_t seed;
    uint8_t  gene[GC_COUNT];
    uint32_t mystery_at;          /* when "Mystery colours" was last chosen */
    uint32_t confirm_at;          /* when the confirm screen opened */
};
static struct gen gen;

/* session-only (not saved, as on the CYD): this pet's egg is a Mystery egg whose shell cycles */
static bool     mystery_body = false;
static uint32_t mystery_body_at = 0;

/* CydGen::Generator::begin: first animal, Coral */
static void gen_begin(void)
{
    rb->memset(&gen, 0, sizeof(gen));
    gen.species = 0;              /* Tropical fish: the radio's fresh pet (freshSpecies) */
    gen.set = 0;
}

static const char *gen_species_text(void) { return gen.mystery ? "Mystery egg" : sp_label(gen.species); }

static void gen_ensure_seed(uint32_t rnd) { if (!gen.seed) gen.seed = rnd ? rnd : 1; }

static void gen_choose_mystery_any(uint32_t rnd)
{
    gen_ensure_seed(rnd);
    gen.mystery = true; gen.set = SET_ANY;
    gen.species = any_mystery(gen.seed);
}

/* row 0 = Mystery egg inside the set, otherwise that animal */
static void gen_choose_species(unsigned set, unsigned row, uint32_t rnd)
{
    if (set >= SET_COUNT) return;
    if (row == 0) {
        gen_ensure_seed(rnd);
        gen.mystery = true; gen.set = (uint8_t)set;
        gen.species = set_mystery(gen.seed, set);
    } else if (row <= set_size(set)) {
        gen.mystery = false; gen.set = (uint8_t)set;
        gen.species = set_selection_id(set, row - 1);
    }
}

static unsigned gen_set_row_now(void)
{
    return gen.mystery && gen.set == SET_ANY ? SET_COUNT : gen.set < SET_COUNT ? gen.set : 0;
}
static unsigned gen_species_row_now(unsigned set)
{
    if (gen.mystery) return 0;
    return set_of(gen.species) == set ? gen.species - set_base(set) + 1 : 1;
}

/* "Mystery colours": the radio's chooseLoRaFishMysteryColours from the session seed; each later
 * press takes a fresh seed (never the old one, never an equal set of colours) */
static void gen_mystery_colours(uint32_t rnd)
{
    int i;
    if (!gen.seed) {
        gen.seed = rnd ? rnd : 1;
    } else if (gen.rolled) {
        uint8_t l0 = gen.gene[GC_ACCENT], l1 = gen.gene[GC_BODY], l2 = gen.gene[GC_EYES];
        uint32_t sd = rnd ? rnd : 1;
        int tries;
        for (tries = 0; tries < 16; ++tries) {
            if (sd != gen.seed && !(mystery_colour(sd, 0) == l0 && mystery_colour(sd, 1) == l1 &&
                                    mystery_colour(sd, 2) == l2))
                break;
            sd = sd * 1664525UL + 1013904223UL;
            if (!sd) sd = 1;
        }
        gen.seed = sd;
    }
    gen.rolled = true;
    for (i = 0; i < GC_COUNT; i++) gen.part_mystery[i] = true;
    gen.gene[GC_ACCENT] = mystery_colour(gen.seed, 0);
    gen.gene[GC_BODY]   = mystery_colour(gen.seed, 1);
    gen.gene[GC_EYES]   = mystery_colour(gen.seed, 2);
}

static bool gen_any_mystery_part(void)
{
    return gen.part_mystery[GC_ACCENT] || gen.part_mystery[GC_BODY] || gen.part_mystery[GC_EYES];
}

/* "Choose colours": one step on in the picker (wraps); that part is chosen by hand */
static void gen_next_colour(unsigned c)
{
    gen.part_mystery[c] = false;
    gen.gene[c] = picker[(picker_index(gen.gene[c]) + 1) % PICKER_COUNT];
}

/* what a screen shows for a part: the chosen gene, or the cycling hue of a mystery part */
static uint8_t gen_shown_gene(unsigned c, uint32_t since_ms)
{
    return gen.part_mystery[c] ? cycle_gene(c, since_ms) : gen.gene[c];
}

/* LoRaFishIdentity::checkName: up to 16 printable ASCII characters; empty = no name. 0 = ok */
static int name_check(const char *n)
{
    size_t i;
    for (i = 0; n[i]; i++) {
        if (i >= NAME_MAX_CHARS) return 1;
        if ((unsigned char)n[i] < 0x20 || (unsigned char)n[i] > 0x7E) return 2;
    }
    return 0;
}

/* CydGen::nameShown: the name, or "(Species)" when there is none */
static void gen_name_shown(char *out, size_t cap)
{
    if (gen.name[0]) rb->snprintf(out, cap, "%s", gen.name);
    else rb->snprintf(out, cap, "(%s)", gen_species_text());
}

/* CydGen::meetText: "Meet Rex!" with a name, "Meet your Dog!" without */
static void gen_meet_text(char *out, size_t cap)
{
    if (gen.name[0]) rb->snprintf(out, cap, "Meet %s!", gen.name);
    else rb->snprintf(out, cap, "Meet your %s!", gen_species_text());
}

/* CydApp::createPet_ via Generator::build: the new pet starts as an egg (age 0, happiness 80) */
static void pet_create(void)
{
    uint32_t seed = mix32(now_ms() ^ rng() ^ (uint32_t)rb->mktime(rb->get_time()));
    if (!seed) seed = 1;
    rb->memset(&pet, 0, sizeof(pet));
    pet.exists = true;
    pet.seed = seed;
    pet.species = gen.species;
    pet.accent = gen.gene[GC_ACCENT];
    pet.body   = gen.gene[GC_BODY];
    pet.eyes   = gen.gene[GC_EYES];
    rb->strlcpy(pet.name, gen.name, sizeof(pet.name));
    pet.mystery = gen.mystery;
    pet.generation = 1;
    pet.happy = START_HAPPINESS;
    mystery_body = gen.part_mystery[GC_BODY];
    mystery_body_at = now_ms();
    pet_dirty = true;
}

/* the colour of an egg's shell: the body gene (CydApp::drawTankEgg_); a mystery body cycles from red */
static uint16_t egg_shell(uint32_t now)
{
    return gene_colour(mystery_body ? cycle_gene(0, now - mystery_body_at) : pet.body);
}

/* next generation (the offspring egg of the manual Fertilise option; solo: all three traits are the parent's) */
static void pet_next_generation(void)
{
    pet.generation = pet.generation < 65535 ? pet.generation + 1 : 65535;
    pet.age_min = 0; pet.age_acc_ms = 0;
    pet.happy = START_HAPPINESS;
    pet.waste = 0; pet.waste_in_ms = 0;
    pet.awake_ms = 0; pet.meal_cd_ms = 0;
    pet.has_egg = false;
    pet.mystery = false;
    pet_dirty = true;
}

/* LoRaFishHappiness::advance, for a step in which the pet is awake (asleep / resting time never reaches it) */
static void pet_decay(uint32_t elapsed)
{
    pet.awake_ms += elapsed;
    while (pet.awake_ms >= DECAY_MS) {
        uint8_t drain = (uint8_t)(1 + (pet.waste > 3 ? 3 : pet.waste));
        pet.awake_ms -= DECAY_MS;
        pet.happy = pet.happy > drain ? (uint8_t)(pet.happy - drain) : 0;
        pet_dirty = true;
    }
}

/* Advance by elapsed ms. resting: asleep or charging. Age goes on while resting (as the radio's
 * does while it sleeps); the happiness decay does not. */
static void pet_tick(uint32_t elapsed, bool resting)
{
    uint32_t mins;
    if (!pet.exists || !elapsed) return;

    pet.meal_cd_ms = elapsed >= pet.meal_cd_ms ? 0 : pet.meal_cd_ms - elapsed;

    if (pet.waste_in_ms) {
        if (elapsed >= pet.waste_in_ms) {
            pet.waste_in_ms = 0;
            if (pet.waste < MAX_WASTE) { pet.waste++; pet_dirty = true; }
        } else {
            pet.waste_in_ms -= elapsed;
        }
    }
    if (pet_living() && !resting)
        pet_decay(elapsed);

    pet.age_acc_ms += elapsed;
    mins = pet.age_acc_ms / 60000UL;
    if (mins) {
        bool was_egg = pet_is_egg();
        pet.age_acc_ms -= mins * 60000UL;
        pet.age_min += mins;
        age_since_save += mins;
        if (was_egg && !pet_is_egg()) { pet_dirty = true; pet.mystery = false; }
    }
    /* succession: the offspring egg takes over five minutes after the memorial */
    if (pet.has_egg && pet.age_min >= LIFESPAN_MINUTES + SUCCESSION_MINUTES)
        pet_next_generation();
}

/* ---- persistence: one small file of 32-bit words with a magic and a checksum ---- */
#define PET_MAGIC1 0x43475031UL  /* "CGP1" (Stage 1, no name): still read */
#define PET_MAGIC 0x43475032UL   /* "CGP2" */
#define PET_WORDS1 20
#define PET_WORDS 24

static void pet_save(void)
{
    uint32_t w[PET_WORDS];
    uint32_t sum = 0;
    int i, fd;
    if (!pet.exists) {                   /* reset: no pet, no file (the radio's reset removes the pet files) */
        rb->remove(PET_FILE);
        pet_dirty = false; age_since_save = 0; pet_last_save = now_ms();
        return;
    }
    rb->memset(w, 0, sizeof(w));
    w[0] = PET_MAGIC;
    w[1] = pet.exists ? 1 : 0;
    w[2] = pet.species; w[3] = pet.body; w[4] = pet.accent; w[5] = pet.eyes;
    w[6] = pet.generation; w[7] = pet.seed; w[8] = pet.mystery ? 1 : 0;
    w[9] = pet.age_min; w[10] = pet.happy; w[11] = pet.waste; w[12] = pet.has_egg ? 1 : 0;
    w[13] = pet.awake_ms; w[14] = pet.meal_cd_ms; w[15] = pet.age_acc_ms; w[16] = pet.waste_in_ms;
    rb->memcpy(&w[17], pet.name, NAME_MAX_CHARS);        /* w[17..20]: the name, zero padded */
    for (i = 0; i < PET_WORDS - 1; i++) sum = sum * 31u + w[i];
    w[PET_WORDS - 1] = sum;
    fd = rb->open(PET_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    rb->write(fd, w, sizeof(w));
    rb->close(fd);
    pet_dirty = false;
    age_since_save = 0;
    pet_last_save = now_ms();
}

static bool pet_load(void)
{
    uint32_t w[PET_WORDS];
    uint32_t sum = 0;
    int i, n, words, fd = rb->open(PET_FILE, O_RDONLY);
    if (fd < 0) return false;
    n = (int)rb->read(fd, w, sizeof(w));
    rb->close(fd);
    if (n == (int)(PET_WORDS1 * 4) && w[0] == PET_MAGIC1) words = PET_WORDS1;
    else if (n == (int)sizeof(w) && w[0] == PET_MAGIC) words = PET_WORDS;
    else return false;
    for (i = 0; i < words - 1; i++) sum = sum * 31u + w[i];
    if (sum != w[words - 1] || !w[1] || !sp_known(w[2])) return false;
    rb->memset(&pet, 0, sizeof(pet));
    pet.exists = true;
    pet.species = (uint16_t)w[2]; pet.body = (uint8_t)w[3]; pet.accent = (uint8_t)w[4];
    pet.eyes = (uint8_t)w[5]; pet.generation = (uint16_t)w[6]; pet.seed = w[7];
    pet.mystery = w[8] != 0; pet.age_min = w[9];
    pet.happy = w[10] > 100 ? 100 : (uint8_t)w[10];
    pet.waste = w[11] > MAX_WASTE ? MAX_WASTE : (uint8_t)w[11];
    pet.has_egg = w[12] != 0;
    pet.awake_ms = w[13] < DECAY_MS ? w[13] : 0;
    pet.meal_cd_ms = w[14] <= MEAL_MS ? w[14] : 0;
    pet.age_acc_ms = w[15] < 60000UL ? w[15] : 0;
    pet.waste_in_ms = w[16];
    if (words == PET_WORDS) {
        rb->memcpy(pet.name, &w[17], NAME_MAX_CHARS);
        pet.name[NAME_MAX_CHARS] = '\0';
        if (name_check(pet.name)) pet.name[0] = '\0';
    }
    mystery_body = false;
    return true;
}

/* ---- care actions (return a message for the owner) ---- */
static const char *pet_feed(void)
{
    if (!pet.exists) return "No pet yet";
    if (pet_is_egg()) return "It is still an egg";
    if (pet_is_memorial()) return "The pet has passed on";
    if (pet.meal_cd_ms) return "Not hungry yet";
    pet.meal_cd_ms = MEAL_MS;
    pet.happy = pet.happy <= 97 ? (uint8_t)(pet.happy + 3) : 100;
    if (!pet.waste_in_ms && pet.waste < MAX_WASTE)
        pet.waste_in_ms = WASTE_MIN_MS + rng() % WASTE_SPREAD_MS;
    pet_dirty = true;
    return "Yum!";
}

static const char *pet_clean(void)
{
    if (!pet.exists || pet.waste == 0) return "Nothing to clean";
    pet.waste = 0; pet.waste_in_ms = 0;
    pet.happy = pet.happy + CLEAN_BONUS > 100 ? 100 : (uint8_t)(pet.happy + CLEAN_BONUS);
    pet_dirty = true;
    return "Squeaky clean";
}

/* Why the pet cannot be fertilised now, or NULL when it can. */
static const char *fertilise_blocked(void)
{
    if (!pet.exists) return "No pet yet";
    if (pet_is_egg()) return "Too young: still an egg";
    if (pet_is_memorial()) return "The pet has passed on";
    if (stage_for_age(pet.age_min) < 2) return "Too young: needs to be an Adult";
    if (pet.has_egg) return "An egg is already waiting";
    return NULL;
}

/* Manual fertilisation (owner decision 2026-11-25): never automatic, never tied to charging or
 * resting. A solo egg: the offspring inherits all three of the parent's traits. Needs an Adult or
 * Elder that is alive and has no egg yet. The egg takes over after the memorial. */
static const char *pet_fertilise(void)
{
    const char *why = fertilise_blocked();
    if (why) return why;
    pet.has_egg = true;
    pet_dirty = true;
    return "An egg was laid. It hatches after this pet's life ends.";
}

/* ------------------------------------------------------------------------
 * Environment: clock, battery, charger, USB
 * ---------------------------------------------------------------------- */
static void fmt_age(char *buf, size_t n, uint32_t mins)
{
    if (mins < 60)            rb->snprintf(buf, n, "%lum", (unsigned long)mins);
    else if (mins < 48 * 60)  rb->snprintf(buf, n, "%luh%02lum", (unsigned long)(mins / 60), (unsigned long)(mins % 60));
    else                      rb->snprintf(buf, n, "%lud%luh", (unsigned long)(mins / 1440), (unsigned long)(mins / 60 % 24));
}

/* the RTC is "set" when it reads a sane year; an iPod with a flat clock battery restarts in 2000 or 1970 */
static bool clock_is_set(const struct tm *t)
{
    return t && t->tm_year + 1900 >= 2020;
}

static bool night_now(const struct tm *t)
{
    int h = t->tm_hour;
    if (!clock_is_set(t) || sleep_hour == wake_hour) return false;
    return sleep_hour > wake_hour ? (h >= sleep_hour || h < wake_hour)
                                  : (h >= sleep_hour && h < wake_hour);
}

struct env {
    bool charging;
    bool resting_charge;   /* charging, rest-while-charging on, and no music */
    bool music;            /* music reactive on and a track playing (not paused) */
    bool low_battery;
    bool asleep;
    bool usb;
    int  battery;
    const struct tm *tm;
};

static void env_read(struct env *e)
{
    e->tm = rb->get_time();
    e->battery = rb->battery_level();
    e->usb = rb->usb_inserted();
#if CONFIG_CHARGING
    e->charging = rb->charger_inserted();
#else
    e->charging = false;
#endif
    e->low_battery = !e->charging && e->battery >= 0 && e->battery <= 20;
    e->music = false;
    if (music_reactive) {
        int st = rb->audio_status();
        e->music = (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);
    }
    e->resting_charge = e->charging && rest_while_charging && !e->music;
    e->asleep = night_now(e->tm) && !e->music;   /* music keeps the pet awake at night too */
}

static const char *status_text(const struct env *e)
{
    if (!pet.exists)       return "No pet";
    if (pet_is_memorial()) return pet.has_egg ? "In memory (egg waits)" : "In memory";
    if (pet_is_egg())      return "Egg";
    if (e->resting_charge) return "Resting (charging)";
    if (e->asleep)         return "Asleep zZ";
    if (e->low_battery)    return "Tired (low battery)";
    if (pet.waste)         return "Needs cleaning";
    if (pet.happy < 30)    return "Sad";
    if (pet.happy >= 80)   return "Happy";
    return "Content";
}

#ifdef HAVE_LCD_COLOR

/* a 565 colour as the target's native pixel */
static unsigned px(uint16_t c)
{
    return LCD_RGBPACK(((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31);
}

static void fr(uint16_t c, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    rb->lcd_set_foreground(px(c));
    rb->lcd_fillrect(x, y, w, h);
}

static void ln(uint16_t c, int x0, int y0, int x1, int y1)
{
    rb->lcd_set_foreground(px(c));
    rb->lcd_drawline(x0, y0, x1, y1);
}

/* ---- scenery (LoRaFishScenery.h, full-bleed: no row of the screen is left unpainted) ---- */
enum kind { K_WATER, K_AIR, K_LAND };

static enum kind kind_for(unsigned id) { return sp_air(id) ? K_AIR : sp_land(id) ? K_LAND : K_WATER; }

static const uint16_t water_band[8] = {0x010B, 0x012D, 0x014F, 0x0170, 0x0190, 0x01B0, 0x01D0, 0x01EF};
static const uint16_t air_sky[8]    = {0x118F, 0x2231, 0x32D3, 0x4375, 0x5416, 0x64D8, 0x757A, 0x8E1C};
static const uint16_t land_sky[8]   = {0x2317, 0x3397, 0x4C18, 0x5CB9, 0x753A, 0x85BB, 0x965C, 0xAEDD};
#define C_SHAFT 0x2B75
#define C_REEF 0x0147
#define C_SAND_DEEP 0x6328
#define C_SAND_MID 0x9C4B
#define C_SAND_LIGHT 0xDDAF
#define C_WEED_A 0x05A8
#define C_WEED_B 0x03C8
#define C_PARTICLE 0x6DBB
#define C_SUN_FILL 0xFEA8
#define C_SUN_EDGE 0xECA4
#define C_SUN_RAY 0xFF71
#define C_CLOUD_LIGHT 0xF7DF
#define C_CLOUD_SHADE 0xBE7D
#define C_AIR_HILL_TOP 0x963A
#define C_AIR_HILL 0x4C14
#define C_MIST_TOP 0xEFBF
#define C_MIST 0xCF1E
#define C_LAND_HILL_TOP 0x7E0C
#define C_LAND_HILL 0x44AA
#define C_GRASS 0x1AE5
#define C_FLOWER 0x9E8B
#define C_TURF_EDGE 0x2B67
#define C_TURF_LIGHT 0x7AA6
#define C_TURF 0x59E5
#define C_TRUNK 0x6A66
#define C_LEAF_DARK 0x2C28
#define C_LEAF_LIGHT 0x556A
#define C_WASTE 0x4A22
#define C_STINK 0x5D6A
#define HEADER_ROWS 17
#define FOOTER_ROWS 15

static void draw_water(int w, int h, uint32_t now)
{
    int i, top = 17, bottom = h - 17, span = bottom - top;
    fr(water_band[0], 0, 0, w, HEADER_ROWS);
    for (i = 0; i < 8; i++) {
        int y0 = top + (span * i) / 8, y1 = top + (span * (i + 1)) / 8;
        fr(water_band[i], 0, y0, w, y1 - y0);
    }
    for (i = 0; i < 5 || 18 + i * 48 - 13 < w; i++) {            /* surface shafts of light */
        int x = 18 + i * 48;
        ln(C_SHAFT, x, 18, x - 13, 39);
        ln(C_SHAFT, x + 3, 18, x - 10, 39);
    }
    for (i = 0; i < 7 || i * 43 - 12 < w; i++) {                 /* far-off reef silhouettes */
        int x = i * 43 - 12, y = h - 31 + (i % 3) * 3;
        fr(C_REEF, x, y, 30, h - 18 - y);
        fr(C_REEF, x + 8, y - 5, 12, 6);
    }
    fr(C_SAND_DEEP, 0, h - 23, w, 6);
    fr(C_SAND_MID, 0, h - 18, w, 3);
    fr(C_SAND_LIGHT, 0, h - 15, w, 1);
    fr(C_SAND_MID, 0, h - 14, w, FOOTER_ROWS - 1);
    for (i = 0; i < 7 || 8 + i * 37 < w; i++) {                  /* seaweed, swaying */
        int x = 8 + i * 37, base = h - 24, tall = 13 + (i % 3) * 5;
        int sway = (int)((now / 480UL + (uint32_t)(i * 3)) % 5U) - 2;
        uint16_t c = (i % 2) ? C_WEED_B : C_WEED_A;
        ln(c, x, base, x + sway, base - tall);
        ln(c, x + 2, base, x + 5 - sway, base - tall + 5);
    }
    for (i = 0; i < 8 || 9 + i * 29 < w; i++) {                  /* rising particles */
        int x = 9 + i * 29;
        int y = 20 + (int)(((uint32_t)(i * 17) + now / 190UL) % (uint32_t)(span > 1 ? span : 1));
        fr(C_PARTICLE, x, y, 1, 1);
    }
}

/* the three bubbles (the radio's tall-panel rows) with a small sideways wobble */
static void draw_bubbles(int w, int h, uint32_t now)
{
    int top = 35, bottom = h - 19;
    unsigned span = (unsigned)((bottom - top + 1) > 1 ? (bottom - top + 1) : 1);
    int y0 = bottom - (int)((now / 180UL) % span);
    int y1 = bottom - (int)((now / 260UL + 13) % span);
    int y2 = bottom - (int)((now / 340UL + 25) % span);
    int x0 = 15 + (sin64((int)((now / 110U) & 63U)) * 2) / 1000;
    int x1 = 29 + (sin64((int)((now / 140U) & 63U)) * 2) / 1000;
    int x2 = w - 18 + (sin64((int)((now / 125U) & 63U)) * 2) / 1000;
    rb->lcd_set_foreground(px(0x8410));
    rb->lcd_drawrect(x0, y0, 2, 2);
    rb->lcd_drawrect(x1, y1, 3, 3);
    rb->lcd_drawrect(x2, y2, 2, 2);
}

static void fill_sky(int w, int h, const uint16_t *bands, int count)
{
    int i, top = HEADER_ROWS, bottom = h - FOOTER_ROWS, span = bottom - top;
    fr(bands[0], 0, 0, w, top);
    for (i = 0; i < count; i++) {
        int y0 = top + (span * i) / count, y1 = top + (span * (i + 1)) / count;
        fr(bands[i], 0, y0, w, y1 - y0);
    }
}

static void draw_sun(int w, int top)
{
    int sx = w - 28, sy = top + 9;
    fr(C_SUN_FILL, sx - 5, sy - 5, 11, 11);
    rb->lcd_set_foreground(px(C_SUN_EDGE));
    rb->lcd_drawrect(sx - 5, sy - 5, 11, 11);
    fr(C_SUN_RAY, sx - 2, sy - 7, 5, 1);
    fr(C_SUN_RAY, sx - 2, sy + 7, 5, 1);
    fr(C_SUN_RAY, sx - 8, sy, 2, 1);
    fr(C_SUN_RAY, sx + 7, sy, 2, 1);
}

static void draw_trees(int w, int h)
{
    int side, floor_y = h - 16;
    if (h <= 64) return;
    for (side = 0; side < 2; ++side) {
        int x = side ? w - 22 : 10;
        fr(C_TRUNK, x, floor_y - 19, 2, 19);
        fr(C_LEAF_DARK, x - 5, floor_y - 15, 12, 2);
        fr(C_LEAF_LIGHT, x - 3, floor_y - 22, 8, 2);
    }
}

static void cloud_puff(int h, int cx, int cy, int cw, int ch)
{
    if (cy + ch > h - 15) ch = h - 15 - cy;
    if (ch > 0) rb->lcd_fillrect(cx, cy, cw, ch);
}

static void draw_air(int w, int h, uint32_t now)
{
    int i, x, top = 28, bottom = h - 18, ridge;
    int drift = (int)((now / 900U) % (unsigned)(w + 48));
    int cloud_x[3], cloud_y[3];
    fill_sky(w, h, air_sky, 8);
    draw_sun(w, top);
    cloud_x[0] = w / 7; cloud_x[1] = w / 2; cloud_x[2] = (w * 5) / 6;
    cloud_y[0] = top + 5; cloud_y[1] = top + 17; cloud_y[2] = top + 8;
    for (i = 0; i < 3; i++) {
        int cx = (cloud_x[i] + w + 48 - (drift / (i + 2))) % (w + 48) - 24;
        int cy = cloud_y[i];
        rb->lcd_set_foreground(px(C_CLOUD_LIGHT));
        cloud_puff(h, cx + 5, cy, 15, 3);
        cloud_puff(h, cx + 1, cy + 3, 24, 3);
        cloud_puff(h, cx - 3, cy + 6, 31, 3);
        rb->lcd_set_foreground(px(C_CLOUD_SHADE));
        cloud_puff(h, cx + 2, cy + 9, 22, 1);
    }
    ridge = bottom - 10;
    for (x = 0; x < w; x += 16) {
        int rise = ((x / 16) % 4 == 0) ? 7 : ((x / 16) % 4 == 1 ? 4 : 2);
        int y = ridge + 8 - rise;
        fr(C_AIR_HILL_TOP, x, y, 16, 1);
        fr(C_AIR_HILL, x, y + 1, 16, bottom - y - 1);
    }
    fr(C_MIST_TOP, 0, bottom, w, 1);
    fr(C_MIST, 0, bottom + 1, w, h - (bottom + 1));
    draw_trees(w, h);
}

static void draw_land(int w, int h)
{
    int x, top = 28, bottom = h - 18, ridge;
    fill_sky(w, h, land_sky, 8);
    draw_sun(w, top);
    ridge = bottom - 10;
    for (x = 0; x < w; x += 16) {
        int rise = ((x / 16) % 4 == 0) ? 7 : ((x / 16) % 4 == 1 ? 4 : 2);
        int y = ridge + 8 - rise;
        fr(C_LAND_HILL_TOP, x, y, 16, 1);
        fr(C_LAND_HILL, x, y + 1, 16, bottom - y - 1);
    }
    for (x = 6; x < w - 6; x += 19) {                            /* grass tufts */
        fr(C_GRASS, x, bottom - 3, 1, 3);
        fr(C_GRASS, x + 4, bottom - 2, 1, 2);
        fr(C_FLOWER, x + 2, bottom - 4, 1, 4);
    }
    fr(C_TURF_EDGE, 0, bottom, w, 1);
    fr(C_TURF_LIGHT, 0, bottom + 1, w, 1);
    fr(C_TURF, 0, bottom + 2, w, h - (bottom + 2));
    draw_trees(w, h);
}

static void draw_scene(enum kind k, int w, int h, uint32_t now)
{
    if (k == K_AIR) draw_air(w, h, now);
    else if (k == K_LAND) draw_land(w, h);
    else { draw_water(w, h, now); draw_bubbles(w, h, now); }
}

/* the waste pile (LoRaFishIdle::wastePile, 9x8 source pixels, drawn at 2x) and its stink trails */
static void draw_waste(int x, int y, uint32_t now, int i)
{
    int wob = (int)((now / 500U + (unsigned)i) & 1U), ph = (int)((now / 400U + (unsigned)i) % 3U);
    fr(C_WASTE, x + (4 + wob) * 2, y, 4, 2);
    fr(C_WASTE, x + 3 * 2, y + 2, 8, 2);
    fr(C_WASTE, x + 2 * 2, y + 4, 10, 2);
    fr(C_WASTE, x + 1 * 2, y + 6, 14, 4);
    fr(C_WASTE, x, y + 10, 18, 4);
    fr(C_WASTE, x + 2, y + 14, 14, 2);
    fr(C_STINK, x + 2 + ph, y - 10, 2, 4);
    fr(C_STINK, x + 12 - ph, y - 14, 2, 4);
}

/* ---- sprite (LoRaFishSpriteRaster::draw: stage sizes by integer resampling, equal pixels in runs) ---- */
#define TANK_SCALE 3                 /* CydTankArt::kTankScale: the pet is a 96x72 box */
#define MASK_W 32
#define MASK_H 24

static unsigned stage_w(int stage) { return stage == 0 ? 20 : stage == 1 ? 26 : 32; }
static unsigned stage_h(int stage) { return stage == 0 ? 15 : stage == 1 ? 19 : 24; }

static unsigned mask_role(const unsigned char *mask, unsigned dx, unsigned sy, unsigned width)
{
    unsigned col = dx * MASK_W / width;
    unsigned bit = sy * MASK_W + col;
    return (mask[bit / 4] >> ((bit % 4) * 2)) & 3;
}

/* (x, y) is the sprite's centre */
static void sprite_draw(int x, int y, bool right, const unsigned char *mask, int stage,
                        const struct colours *c, int scale)
{
    unsigned width = stage_w(stage), height = stage_h(stage), dy, dx;
    unsigned pal[4];
    pal[0] = 0;
    pal[1] = px(c->body); pal[2] = px(c->fins); pal[3] = px(c->eyes);
    for (dy = 0; dy < height; ++dy) {
        unsigned sy = dy * MASK_H / height;
        for (dx = 0; dx < width;) {
            unsigned start = dx, role = mask_role(mask, dx, sy, width);
            int left;
            dx++;
            while (dx < width && mask_role(mask, dx, sy, width) == role) ++dx;
            if (!role) continue;
            left = right ? (int)start - (int)width / 2 : (int)width / 2 - (int)dx + 1;
            rb->lcd_set_foreground(pal[role]);
            rb->lcd_fillrect(x + left * scale, y + ((int)dy - (int)height / 2) * scale,
                             (int)(dx - start) * scale, scale);
        }
    }
}

/* ---- egg, hatch, memorial (LoRaFishLifeArt.h; pixel rule of CydTankArt::drawEgg) ---- */
#define EGG_H 18
typedef void (*pixfn)(int x, int y, unsigned role);

static int art_cx, art_top, art_scale;
static uint16_t art_c0, art_c2;           /* role 0 and role 2 colours; role 1 is dark ink */

static void art_pixel(int x, int y, unsigned role)
{
    uint16_t c = role == 0 ? art_c0 : role == 2 ? art_c2 : 0x0000;
    fr(c, art_cx + (x - art_cx) * art_scale, art_top + (y - art_top) * art_scale, art_scale, art_scale);
}

static const unsigned char egg_radius[18] = {2,3,4,5,5,6,6,7,7,7,7,7,7,6,6,5,4,2};

static void life_egg(pixfn pixel, int cx, int top, uint32_t key, unsigned progress, uint32_t now, bool mystery)
{
    int phase = (int)((now / 280U) % 4U);
    int tilt = phase == 1 ? -1 : phase == 3 ? 1 : 0;
    int x, y, sx, sy;
    unsigned i, length;
#define DOT(dx_, dy_, role_) pixel(cx + (dx_) + tilt * (17 - (dy_)) / 8, top + (dy_), (role_))
    for (y = 0; y < 18; ++y)
        for (x = -egg_radius[y]; x <= egg_radius[y]; ++x) {
            bool edge = x == -egg_radius[y] || x == egg_radius[y] || y == 0 || y == 17;
            DOT(x, y, edge ? 1 : 0);
        }
    for (i = 0; i < 9; ++i) {                                   /* stable speckles */
        uint32_t n = mix32(key + i * 0x9e3779b9UL);
        y = 3 + (int)(n % 12); x = (int)((n >> 8) % 9) - 4;
        if (!mystery || x <= -3 || x >= 3) DOT(x, y, 2);
    }
    length = progress >= 80 ? 15 : progress >= 60 ? 10 : progress >= 40 ? 6 : progress >= 20 ? 3 : 0;
    for (i = 0; i < length; ++i) {                              /* cracks */
        x = (i % 4 < 2 ? -1 : 0);
        if (!mystery || i < 4 || i > 12) DOT(x, (int)i + 1, 1);
        if (i > 7 && progress >= 80) DOT(x + 1 + ((int)i - 8) / 2, (int)i + 1, 1);
    }
    if (mystery) {
        static const unsigned char question[7] = {14, 17, 1, 2, 4, 0, 4};
        for (y = 0; y < 7; ++y)
            for (x = 0; x < 5; ++x)
                DOT(x - 2, y + 5, (question[y] & (1 << (4 - x))) ? 1 : 0);
    }
    sx = (phase & 1) ? -11 : 11; sy = phase < 2 ? 4 : 12;       /* glitter */
    pixel(cx + sx, top + sy, 2);
    if (phase & 1) {
        pixel(cx + sx - 1, top + sy, 2); pixel(cx + sx + 1, top + sy, 2);
        pixel(cx + sx, top + sy - 1, 2); pixel(cx + sx, top + sy + 1, 2);
    }
#undef DOT
}

static void life_hatch(pixfn pixel, int cx, int top, unsigned phase, uint32_t now)
{
    int opening, lift, x, y;
    if (phase > 4) phase = 4;
    opening = (int)phase * 2; lift = (int)phase;
    for (y = 0; y < 18; ++y)
        for (x = -egg_radius[y]; x <= egg_radius[y]; ++x) {
            bool left = x <= (y % 4 < 2 ? -1 : 0);
            int dx = left ? -opening : opening;
            int dy = y < 10 ? -lift : 0;
            bool edge = x == -egg_radius[y] || x == egg_radius[y] || y == 0 || y == 17 ||
                        (y < 12 && (left ? x == (y % 4 < 2 ? -1 : 0) : x == (y % 4 < 2 ? 0 : 1)));
            if (y >= 11 && phase >= 3) pixel(cx + x, top + y, edge ? 1 : 0);
            else pixel(cx + x + dx, top + y + dy, edge ? 1 : 0);
        }
    if (phase >= 2) {
        int rise = ((int)phase - 1) * 3, hy = top + 13 - rise;
        for (y = 0; y < 7; ++y)
            for (x = -5; x <= 5; ++x) {
                int r = (y == 0 || y == 6) ? 3 : 5;
                bool edge;
                if (x < -r || x > r) continue;
                edge = x == -r || x == r || y == 0 || y == 6;
                pixel(cx + x, hy + y, edge ? 1 : 0);
            }
        pixel(cx - 2, hy + 3, 1); pixel(cx + 2, hy + 3, 1);
        pixel(cx, hy + 5, 1);
    }
    if (phase >= 3 && ((now / 240U) & 1U)) {
        pixel(cx - 16, top + 5, 2); pixel(cx + 16, top + 5, 2);
    }
}

static void life_memorial(pixfn pixel, int cx, int top, unsigned variant)
{
    static const unsigned char letters[3][5] = {{6,5,6,5,5}, {7,2,2,2,7}, {6,5,6,4,4}};
    int x, y, l;
    variant %= 3;
    for (y = 0; y < 25; ++y)
        for (x = -12; x <= 12; ++x) {
            int half = variant == 0 ? (y < 2 ? 5 : y < 5 ? 8 : y < 22 ? 10 : 12) :
                       variant == 1 ? (y < 3 ? 2 + y : y < 20 ? 7 : y < 23 ? 10 : 12) :
                                      (y < 3 ? 5 : y < 7 ? 8 : y < 11 ? 12 : y < 22 ? 8 : 12);
            if (x >= -half && x <= half) {
                bool edge = x == -half || x == half || y == 0 || y == 24;
                pixel(cx + x, top + y, edge ? 1 : 0);
            }
        }
    for (l = 0; l < 3; ++l)
        for (y = 0; y < 5; ++y)
            for (x = 0; x < 3; ++x)
                if (letters[l][y] & (1 << (2 - x))) pixel(cx - 5 + l * 4 + x, top + 12 + y, 1);
    pixel(cx, top + 6, 2); pixel(cx - 1, top + 7, 2); pixel(cx + 1, top + 7, 2);
    pixel(cx, top + 8, 2);
}

/* LoRaFishLifeArt::identity: stable key of this pet for speckles / headstone */
static uint32_t pet_key(void)
{
    return mix32(pet.seed ^ ((uint32_t)pet.generation << 16) ^ pet.species ^
                 ((uint32_t)pet.body << 24) ^ ((uint32_t)pet.accent << 8) ^ pet.eyes);
}

/* ---- text: white with a black shadow, drawn over the scene ---- */
static void text_at(int x, int y, const char *s)
{
    rb->lcd_set_foreground(LCD_BLACK);
    rb->lcd_putsxy(x + 1, y + 1, (const unsigned char *)s);
    rb->lcd_set_foreground(LCD_WHITE);
    rb->lcd_putsxy(x, y, (const unsigned char *)s);
}

static void text_right(int right, int y, const char *s)
{
    int w, h;
    rb->lcd_getstringsize((const unsigned char *)s, &w, &h);
    text_at(right - w, y, s);
}

static void text_center(int cx, int y, const char *s)
{
    int w, h;
    rb->lcd_getstringsize((const unsigned char *)s, &w, &h);
    text_at(cx - w / 2, y, s);
}

/* ---- the pet's wander (a small stand-in for LoRaFishSpecies::naturalidle) ----
 * Position along the roam span in 1/16 pixel; ground walkers stand and walk, flyers cruise, swimmers
 * drift and hover. Crossing times, stage scaling and the gait ranges are the radio's. */
struct mover {
    bool     init;
    int32_t  xq;            /* 0 .. span*16 */
    int      dir;           /* +1 right, -1 left */
    bool     moving;
    uint32_t left_ms;       /* time left in this phase */
    uint32_t last_ms;
    int      hover;         /* swimmer in hover (slow wobble) */
};
static struct mover mv;

/* roam crossing time of the species at this life stage, ms (half the radio's period) */
static uint32_t crossing_ms(unsigned id, int stage)
{
    uint32_t period;
    switch (sp_movement(id)) {
    case MV_CRAWL:   period = 16000U; break;
    case MV_WALK:    period = 10000U; break;
    case MV_FLUTTER: period = 4200U; break;
    case MV_FLY:     period = 8000U; break;
    default:         period = id == ID_CRAB ? 12000U : id == ID_SHELLDWELLER ? 32000U : 16000U; break;
    }
    if (stage == 0) period = period * 3U / 4U;
    else if (stage >= 3) period = period * 5U / 4U;
    period /= 2U;
    if (id == ID_PENGUIN) period = period * 10U / 3U;           /* waddles at 30% */
    return period;
}

static bool floor_walker(unsigned id)
{
    enum movement m = sp_movement(id);
    return m == MV_WALK || m == MV_CRAWL || id == ID_CRAB || id == ID_SHELLDWELLER;
}

static void mover_update(unsigned id, int stage, int span, uint32_t now, bool frozen)
{
    uint32_t dt = now - mv.last_ms;
    int32_t spanq = span * 16;
    enum movement m = sp_movement(id);
    bool ground = floor_walker(id);
    uint32_t cross = crossing_ms(id, stage);
    int32_t step;

    mv.last_ms = now;
    if (!mv.init) {
        mv.init = true;
        mv.xq = (int32_t)(rng() % (uint32_t)(spanq > 0 ? spanq : 1));
        mv.dir = (rng() & 1) ? 1 : -1;
        mv.moving = false; mv.left_ms = rng_range(1000, 3000); mv.hover = 0;
    }
    if (dt > 250) dt = 250;                                    /* a long gap (menu, splash): carry on from here */
    if (frozen || span <= 0) return;

    step = (int32_t)((uint32_t)spanq * dt / (cross ? cross : 1));
    if (mv.left_ms > dt) mv.left_ms -= dt; else mv.left_ms = 0;

    if (ground) {
        if (mv.moving) {
            mv.xq += mv.dir * step;
            if (mv.xq <= 0 || mv.xq >= spanq) {                 /* wall: stop, turn round, look around */
                mv.xq = mv.xq <= 0 ? 0 : spanq;
                mv.dir = -mv.dir; mv.moving = false;
                mv.left_ms = rng_range(1400, 2600);
            } else if (!mv.left_ms) {
                mv.moving = false;
                mv.left_ms = m == MV_CRAWL ? rng_range(1500, 4500) : rng_range(1000, 3000);
            }
        } else if (!mv.left_ms) {
            mv.moving = true;
            mv.left_ms = m == MV_CRAWL ? rng_range(1800, 4000) : rng_range(2000, 4500);
            if (mv.xq < 160) mv.dir = 1;                        /* never start a walk into a wall */
            else if (mv.xq > spanq - 160) mv.dir = -1;
            else if (rng() % 100 < 35) mv.dir = -mv.dir;        /* sometimes turn first */
        }
    } else if (m == MV_FLY || m == MV_FLUTTER) {                /* cruise, reverse at the walls and now and then */
        mv.moving = true;
        mv.xq += mv.dir * step;
        if (mv.xq <= 0 || mv.xq >= spanq) {
            mv.xq = mv.xq <= 0 ? 0 : spanq;
            mv.dir = -mv.dir;
            mv.left_ms = m == MV_FLUTTER ? rng_range(700, 1800) : rng_range(1800, 4200);
        } else if (!mv.left_ms) {
            if (rng() % 100 < 45) mv.dir = -mv.dir;             /* mid-air turn */
            mv.left_ms = m == MV_FLUTTER ? rng_range(700, 1800) : rng_range(1800, 4200);
        }
    } else {                                                    /* swimmer: drift, then hover; never stops */
        if (!mv.hover) {
            mv.xq += mv.dir * step;
            mv.moving = true;
            if (mv.xq <= 0 || mv.xq >= spanq) {
                mv.xq = mv.xq <= 0 ? 0 : spanq;
                mv.dir = -mv.dir; mv.hover = 1; mv.left_ms = rng_range(1500, 2800);
            } else if (!mv.left_ms) {
                mv.hover = 1; mv.left_ms = rng_range(1500, 2800);
                if (rng() % 100 < 10) mv.dir = -mv.dir;
            }
        } else {
            mv.xq += mv.dir * (step / 4);                       /* slow while hovering */
            if (mv.xq < 0) mv.xq = 0;
            if (mv.xq > spanq) mv.xq = spanq;
            if (!mv.left_ms) { mv.hover = 0; mv.left_ms = rng_range(3000, 6000); }
        }
    }
}

/* Water tank bands (LoRaFishSpecies::band / bandFit, tall layout): surface 18..29, underwater 30..42 */
static bool surface_band(unsigned id)
{
    return id == ID_DOLPHIN || id == ID_WHALE || id == ID_SEAL || id == ID_JELLYFISH;
}

static int cruise_row(uint32_t t)           /* LoRaFishIdle::cruiseRow: 30 + 11*sin(t / 5100) */
{
    return 30 + 11 * sin64((int)((t / 500U) & 63U)) / 1000;
}

/* ---- the home screen ---- */
static int line_h = 8;

/* Track metadata for the screens (RockBox audio_current_track). NULL when nothing plays or pauses. */
static const char *track_line(char *buf, size_t len)
{
    struct mp3entry *id3;
    if (!(rb->audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))) return NULL;
    id3 = rb->audio_current_track();
    if (!id3) return NULL;
    if (id3->artist && id3->title)      rb->snprintf(buf, len, "%s - %s", id3->artist, id3->title);
    else if (id3->title)                rb->snprintf(buf, len, "%s", id3->title);
    else                                return NULL;
    return buf;
}

/* Stereo level meter: two vertical bars, left at the left edge and right at the right edge,
   rising from the bottom. Peaks scaled 0..65536 (oscilloscope's scale). Drawn before the pet so the pet covers it. */
#define METER_W 6
static void draw_meters(int w, int top, int bottom)
{
    static struct pcm_peaks peaks;
    unsigned l, r;
    int span = bottom - top, fl, fr_h;
    if (span < 2) return;
    rb->mixer_channel_calculate_peaks(PCM_MIXER_CHAN_PLAYBACK, &peaks);
    l = peaks.left  > 65535 ? 65535 : peaks.left;
    r = peaks.right > 65535 ? 65535 : peaks.right;
    fl   = (int)(l * (unsigned)span / 65536U);
    fr_h = (int)(r * (unsigned)span / 65536U);
    rb->lcd_set_foreground(px(0x2104));                       /* dark track, both channels */
    rb->lcd_fillrect(2, top, METER_W, span);
    rb->lcd_fillrect(w - 2 - METER_W, top, METER_W, span);
    rb->lcd_set_foreground(px(0x07E0));                       /* green level from the bottom */
    if (fl > 0)   rb->lcd_fillrect(2, bottom - fl, METER_W, fl);
    if (fr_h > 0) rb->lcd_fillrect(w - 2 - METER_W, bottom - fr_h, METER_W, fr_h);
}

/* Track progress bar on the sand layer, below the ground pets and the seaweed (rows h-16..h-15). */
static void draw_track_progress(int w, int h)
{
    struct mp3entry *id3;
    int span = w - 16, pw;
    if (!(rb->audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))) return;
    id3 = rb->audio_current_track();
    if (!id3 || id3->length == 0) return;
    pw = (int)((unsigned long long)id3->elapsed * (unsigned)span / id3->length);
    if (pw > span) pw = span;
    rb->lcd_set_foreground(px(0x2104));
    rb->lcd_fillrect(8, h - 16, span, 2);
    rb->lcd_set_foreground(px(0x07E0));
    if (pw > 0) rb->lcd_fillrect(8, h - 16, pw, 2);
}

static void draw_pet_home(const struct env *e, uint32_t now)
{
    int w = LCD_WIDTH, h = LCD_HEIGHT, i;
    int hdr = 2 * line_h + 3;
    int k_top = hdr + 2 > 28 ? hdr + 2 : 28;           /* the pet's top is never above this */
    int floor_gap = 18;
    int stage = stage_for_age(pet.age_min);
    char buf[64], buf2[32];
    enum kind k = pet.exists ? kind_for(pet.species) : K_WATER;
    bool frozen = !pet_living() || e->resting_charge || e->asleep || e->low_battery;

    draw_scene(k, w, h, now);
    if (e->music) {                                           /* scenery layer: meters, then progress, then the pet over both */
        draw_meters(w, hdr + 4, h - 26);
    }
    draw_track_progress(w, h);

    for (i = 0; i < pet.waste; i++)
        draw_waste(30 + i * 90, h - 20 - 16, now, i);

    if (!pet.exists) {
        text_center(w / 2, h / 2 - line_h, "No pet yet");
    } else if (pet_is_memorial()) {
        art_scale = TANK_SCALE; art_c0 = 0xC618; art_c2 = gene_colour(pet.body);
        art_cx = w / 2; art_top = h - floor_gap - 25 * TANK_SCALE;
        life_memorial(art_pixel, art_cx, art_top, pet_key() % 3);
        if (pet.has_egg) {                                     /* the offspring egg waits beside the stone */
            art_cx = w / 2 + 70; art_top = h - floor_gap - EGG_H * TANK_SCALE;
            art_c0 = gene_rgb565(8 + pet_key() % 240); art_c2 = 0xFFFF;
            life_egg(art_pixel, art_cx, art_top, pet_key(), 0, now, false);
        }
    } else if (pet_is_egg()) {
        uint32_t into = (uint32_t)pet.age_min * 60000UL + pet.age_acc_ms;
        uint32_t total = EGG_MINUTES * 60000UL, rem = total - into;
        art_scale = TANK_SCALE; art_c0 = egg_shell(now); art_c2 = 0xFFFF;
        art_cx = w / 2; art_top = h - floor_gap - EGG_H * TANK_SCALE - 6;
        if (rem <= HATCH_MS)
            life_hatch(art_pixel, art_cx, art_top, (unsigned)((HATCH_MS - rem) / HATCH_PHASE_MS), now);
        else
            life_egg(art_pixel, art_cx, art_top, pet_key(), (unsigned)(into * 100UL / total), now, pet.mystery);
    } else {
        struct colours c;
        const unsigned char *mask;
        unsigned frame;
        int sw, sh, cx_min, cx_max, c_min, c_max, cx, cy, logical_x, cx_span;
        sw = (int)stage_w(stage) * TANK_SCALE; sh = (int)stage_h(stage) * TANK_SCALE;
        cx_min = 8 + sw / 2; cx_max = w - 8 - sw / 2;
        c_min = k_top + sh / 2; c_max = h - floor_gap - sh / 2;
        cx_span = cx_max - cx_min;
        mover_update(pet.species, stage, cx_span, now, frozen);
        cx = cx_min + mv.xq / 16;
        if (floor_walker(pet.species) || (frozen && sp_movement(pet.species) != MV_SWIM)) {
            cy = c_max;                                         /* walkers stand on the floor; resting flyers settle */
        } else {
            int row = cruise_row(now);
            if (sp_water(pet.species)) {
                int top_r = surface_band(pet.species) ? 18 : 30, bot_r = surface_band(pet.species) ? 29 : 42;
                if (row < 19) row = 19;
                if (row > 41) row = 41;
                row = top_r + (row - 19) * (bot_r - top_r) / (41 - 19);
            }
            if (row < 18) row = 18;
            if (row > 42) row = 42;
            cy = c_min + (row - 18) * (c_max - c_min) / (42 - 18);
        }
        /* frame: ground walkers step by distance walked, the rest flap / wobble on the clock */
        logical_x = 20 + mv.xq * 48 / (cx_span > 0 ? cx_span * 16 : 1);
        if (frozen) frame = 0;
        else if (sp_movement(pet.species) == MV_WALK || sp_movement(pet.species) == MV_CRAWL)
            frame = (unsigned)(((logical_x < 0 ? -logical_x : logical_x) / 5) & 1);
        else frame = (unsigned)((mv.hover ? now / 2U : now) / 280U) & 1U;
        c = colours_for(pet.body, pet.accent, pet.eyes, stage);
        mask = sp_mask(pet.species, frame);
        if (mask) sprite_draw(cx, cy, mv.dir > 0, mask, stage, &c, TANK_SCALE);
        if (e->asleep || e->resting_charge) text_center(cx, cy - sh / 2 - line_h, "z Z z");
        else if (e->low_battery) text_center(cx, cy - sh / 2 - line_h, "...");
    }

    /* header: species and stage, clock, battery, USB, status */
    if (pet.exists) {
        const char *nm = pet.name[0] ? pet.name : sp_label(pet.species);
        if (pet_is_egg()) rb->snprintf(buf, sizeof(buf), "%s", pet.name[0] ? pet.name : pet.mystery ? "Mystery egg" : "Egg");
        else if (pet_is_memorial()) rb->snprintf(buf, sizeof(buf), "%s (gen %u)", nm, pet.generation);
        else rb->snprintf(buf, sizeof(buf), "%s %s", nm, sp_stage_name(pet.species, stage));
    } else rb->snprintf(buf, sizeof(buf), "ClickGotchi");
    text_at(2, 1, buf);

    if (clock_is_set(e->tm)) {
        if (show_seconds) rb->snprintf(buf, sizeof(buf), "%02d:%02d:%02d", e->tm->tm_hour, e->tm->tm_min, e->tm->tm_sec);
        else rb->snprintf(buf, sizeof(buf), "%02d:%02d", e->tm->tm_hour, e->tm->tm_min);
    } else rb->snprintf(buf, sizeof(buf), "Time not set");
    text_right(w - 2, 1, buf);

    rb->snprintf(buf, sizeof(buf), "Bat %d%%%s USB %s", e->battery, e->charging ? " chg" : "", e->usb ? "on" : "off");
    text_at(2, 1 + line_h, buf);
    text_right(w - 2, 1 + line_h, status_text(e));

    /* footer */
    fmt_age(buf2, sizeof(buf2), pet.age_min);
    if (pet.exists)
        rb->snprintf(buf, sizeof(buf), "Happy %d  Age %s  Starts %ld%s", pet.happy, buf2, boot_count,
                     pet.has_egg ? "  +egg" : "");
    else
        rb->snprintf(buf, sizeof(buf), "Starts %ld", boot_count);
    text_at(2, h - line_h - 1, buf);
    text_right(w - 2, h - line_h - 1, "MENU care");

    /* track metadata above the footer; the level meter (music reactive) above that, at the right */
    if (track_line(buf, sizeof(buf)))
        text_at(2, h - 2 * line_h - 2, buf);
}

#endif /* HAVE_LCD_COLOR */

/* The compact baseline layout: 6 text rows, fits 128x64. Used on screens without colour. */
static void put_row(int row, int lh, const char *text)
{
    rb->lcd_putsxy(0, row * lh, (const unsigned char *)text);
}

#ifndef HAVE_LCD_COLOR
static int line_h = 8;
#endif

static void draw_compact(const struct env *e)
{
    char buf[40], a[16];
    int stage = stage_for_age(pet.age_min);
    if (pet.exists && !pet_is_egg() && !pet_is_memorial())
        rb->snprintf(buf, sizeof(buf), "%s %s", sp_label(pet.species), sp_stage_name(pet.species, stage));
    else
        rb->snprintf(buf, sizeof(buf), "ClickGotchi: %s", pet.exists ? (pet_is_egg() ? "egg" : "memorial") : "no pet");
    put_row(0, line_h, buf);

    if (clock_is_set(e->tm)) {
        if (show_seconds) rb->snprintf(buf, sizeof(buf), "Time %02d:%02d:%02d", e->tm->tm_hour, e->tm->tm_min, e->tm->tm_sec);
        else rb->snprintf(buf, sizeof(buf), "Time %02d:%02d", e->tm->tm_hour, e->tm->tm_min);
    } else rb->snprintf(buf, sizeof(buf), "Time not set");
    put_row(1, line_h, buf);

    rb->snprintf(buf, sizeof(buf), "Bat %d%% %s", e->battery, e->charging ? "chg" : "");
    put_row(2, line_h, buf);
    rb->snprintf(buf, sizeof(buf), "USB %s %s", e->usb ? "on" : "off", status_text(e));
    put_row(3, line_h, buf);
    fmt_age(a, sizeof(a), pet.age_min);
    rb->snprintf(buf, sizeof(buf), "Hp%d %s St%ld", pet.happy, a, boot_count);
    put_row(4, line_h, buf);
    put_row(5, line_h, "MENU care|CANCEL");
}

static void draw_screen(void)
{
    struct env e;
    int w, h;
    env_read(&e);
    rb->lcd_getstringsize((const unsigned char *)"A", &w, &h);
    line_h = h;
#ifdef HAVE_LCD_COLOR
    if (LCD_WIDTH >= 240 && LCD_HEIGHT >= 160) {
        rb->lcd_set_drawmode(DRMODE_FG);
        draw_pet_home(&e, now_ms());
        rb->lcd_set_drawmode(DRMODE_SOLID);
        rb->lcd_update();
        return;
    }
#endif
    rb->lcd_clear_display();
    draw_compact(&e);
    rb->lcd_update();
}

/* ------------------------------------------------------------------------
 * Menus
 * ---------------------------------------------------------------------- */
/* A RockBox list screen. Returns the chosen row, or -1 for CANCEL (or USB: usb_pending is set). */
static bool usb_pending = false;

typedef const char *(*row_fn)(int item, void *data, char *buf, size_t len);

static int pick_list(const char *title, int count, row_fn get_row, int *sel)
{
    struct simplelist_info info;
    rb->simplelist_info_init(&info, (char *)title, count, NULL);
    info.get_name = get_row;
    info.selection = (sel && *sel >= 0 && *sel < count) ? *sel : 0;
    if (rb->simplelist_show_list(&info)) { usb_pending = true; return -1; }
    if (info.selection >= 0 && sel) *sel = info.selection;
    return info.selection;
}

static void refresh_line_h(void)
{
    int w, h;
    rb->lcd_getstringsize((const unsigned char *)"A", &w, &h);
    line_h = h;
}

/* ---- plain full-screen pages (preview, reset prompt): black page, white text ---- */
#ifdef HAVE_LCD_COLOR
static void scr_begin(void)
{
    rb->lcd_set_drawmode(DRMODE_FG);
    fr(0x0000, 0, 0, LCD_WIDTH, LCD_HEIGHT);
}
static void scr_text(int x, int y, const char *s) { text_at(x, y, s); }
static void scr_end(void)
{
    rb->lcd_set_drawmode(DRMODE_SOLID);
    rb->lcd_update();
}
#else
static void scr_begin(void) { rb->lcd_clear_display(); }
static void scr_text(int x, int y, const char *s) { rb->lcd_putsxy(x, y, (const unsigned char *)s); }
static void scr_end(void) { rb->lcd_update(); }
#endif

/* ---- Adopt a pet > New pet > generator (CydApp.h: drawAdopt_, drawGenMain_, genActivate_) ---- */
static int gen_list_set = 0;      /* the set whose animals the species list shows */

static const char *adopt_row(int item, void *data, char *buf, size_t len)
{
    (void)item; (void)data; (void)buf; (void)len;
    return "New pet";
}

static const char *genmain_row(int item, void *data, char *buf, size_t len)
{
    char shown[40];
    (void)data;
    switch (item) {
    case 0: rb->snprintf(buf, len, "Species: %s", gen_species_text()); break;
    case 1: gen_name_shown(shown, sizeof(shown)); rb->snprintf(buf, len, "Name: %s", shown); break;
    case 2: return gen_any_mystery_part() ? "Mystery colours (on)" : "Mystery colours";
    case 3: return "Choose colours";
    default: return "Start pet";
    }
    return buf;
}

static const char *genset_row(int item, void *data, char *buf, size_t len)
{
    (void)data; (void)buf; (void)len;
    return item >= SET_COUNT ? "Mystery egg" : set_label((unsigned)item);
}

static const char *genspecies_row(int item, void *data, char *buf, size_t len)
{
    (void)data; (void)buf; (void)len;
    return item == 0 ? "Mystery egg" : sp_label(set_selection_id((unsigned)gen_list_set, (unsigned)item - 1));
}

static const char *gencolour_row(int item, void *data, char *buf, size_t len)
{
    static const char *const parts[GC_COUNT] = {"Accent", "Body", "Eyes"};
    (void)data;
    if (item >= GC_COUNT) return "Done";
    rb->snprintf(buf, len, "%s: %s", parts[item], gen.part_mystery[item] ? "Mystery" : colour_name(gen.gene[item]));
    return buf;
}

/* Species: Water / Air / Land / Mystery egg, then the animals of the set (Mystery egg first) */
static void gen_pick_species(void)
{
    int set_sel = (int)gen_set_row_now();
    while (!usb_pending) {
        int r = pick_list("Species", SET_COUNT + 1, genset_row, &set_sel);
        int row, q;
        if (r < 0) return;
        if (r >= SET_COUNT) { gen_choose_mystery_any(rng()); return; }
        gen_list_set = r;
        row = (int)gen_species_row_now((unsigned)r);
        q = pick_list(set_label((unsigned)r), (int)set_size((unsigned)r) + 1, genspecies_row, &row);
        if (q >= 0) { gen_choose_species((unsigned)r, (unsigned)q, rng()); return; }
        /* CANCEL on the animals goes back to the set list, as on the CYD */
    }
}

/* Name: the radio's rules (up to 16 printable ASCII characters; empty = no name of its own) */
static bool ask_name(char *name_out)
{
    char buf[NAME_MAX_CHARS + 1];
    rb->strlcpy(buf, name_out, sizeof(buf));
    if (rb->kbd_input(buf, sizeof(buf), NULL) < 0) return false;
    if (name_check(buf)) {
        rb->splash(HZ * 2, "A name is up to 16 plain characters");
        return false;
    }
    rb->strlcpy(name_out, buf, NAME_MAX_CHARS + 1);
    return true;
}

static void gen_pick_colours(void)
{
    int sel = 0;
    while (!usb_pending) {
        int r = pick_list("Choose colours", GC_COUNT + 1, gencolour_row, &sel);
        if (r < 0 || r >= GC_COUNT) return;
        gen_next_colour((unsigned)r);
    }
}

/* "Start this pet?" (CydApp::drawGenConfirm_): the preview, with a mystery part shown cycling */
static void draw_gen_confirm(void)
{
    static const char *const parts[GC_COUNT] = {"Accent", "Body", "Eyes"};
    uint32_t now = now_ms(), since = now - gen.confirm_at;
    int h = LCD_HEIGHT, i, y, row;
    char shown[40], line[40];

    refresh_line_h();
    row = line_h + 3;
    scr_begin();
    scr_text(2, 1, "Start this pet?");
#ifdef HAVE_LCD_COLOR
    {
        int cx = LCD_WIDTH - 4 - 48, cy = h / 2;
        if (gen.mystery) {          /* a Mystery egg is not revealed: the question mark, shell = body colour */
            art_scale = TANK_SCALE;
            art_c0 = gene_colour(gen.part_mystery[GC_BODY] ? cycle_gene(0, since) : gen.gene[GC_BODY]);
            art_c2 = 0xFFFF;
            art_cx = cx; art_top = cy - EGG_H * TANK_SCALE / 2;
            life_egg(art_pixel, art_cx, art_top, 0, 0, now, true);
        } else {
            struct colours c = colours_for(gen_shown_gene(GC_BODY, since), gen_shown_gene(GC_ACCENT, since),
                                           gen_shown_gene(GC_EYES, since), 2);
            const unsigned char *mask = sp_mask(gen.species, 0);
            if (mask) sprite_draw(cx, cy, true, mask, 2, &c, TANK_SCALE);
        }
    }
#endif
    y = 2 * row;
    gen_name_shown(shown, sizeof(shown));
    scr_text(2, y, shown);
    scr_text(2, y + row, gen_species_text());
    for (i = 0; i < GC_COUNT; i++) {
        rb->snprintf(line, sizeof(line), "%s: %s", parts[i], gen.part_mystery[i] ? "Mystery" : colour_name(gen.gene[i]));
        scr_text(2, y + (3 + i) * row, line);
#ifdef HAVE_LCD_COLOR
        fr(gene_colour(gen_shown_gene((unsigned)i, since)), 130, y + (3 + i) * row, 14, line_h);
#endif
    }
    scr_text(2, h - line_h - 2, "SELECT: start   MENU: back");
    scr_end();
}

static bool gen_confirm(void)
{
    gen.confirm_at = now_ms();
    while (!usb_pending) {
        int action;
        draw_gen_confirm();
        action = rb->get_action(CONTEXT_STD, HZ / 10);
        if (action == ACTION_STD_OK) return true;
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU) return false;
        if (rb->default_event_handler(action) == SYS_USB_CONNECTED) usb_pending = true;
    }
    return false;
}

/* Generator main page. Returns true when a pet was created (only from the confirm page's OK). */
static bool gen_flow(void)
{
    int sel = 0;
    gen_begin();
    while (!usb_pending) {
        int r = pick_list("New pet", 5, genmain_row, &sel);
        if (r < 0) return false;                      /* nothing was created */
        switch (r) {
        case 0: gen_pick_species(); break;
        case 1: ask_name(gen.name); break;
        case 2: gen_mystery_colours(rng()); gen.mystery_at = now_ms(); break;
        case 3: gen_pick_colours(); break;
        default:
            if (gen_confirm()) {
                char msg[48];
                pet_create();
                mv.init = false;
                gen_meet_text(msg, sizeof(msg));
                pet_save();
                rb->splash(HZ * 2, msg);
                return true;
            }
            break;
        }
    }
    return false;
}

/* The no-pet screen: "Adopt a pet". CANCEL leaves the plugin (the CYD's Back has nowhere else to go
 * here); "From another device" is not offered (no transfer on the iPod). */
static bool adopt_flow(void)
{
    int sel = 0;
    while (!pet.exists && !usb_pending) {
        int r = pick_list("Adopt a pet", 1, adopt_row, &sel);
        if (r < 0) return false;
        if (gen_flow()) return true;
    }
    return pet.exists;
}

/* ---- Reset pet: PetResetConfirmation (Cancel by default; Reset must be selected, then confirmed
 * while the prompt shows; every exit disarms) with the CYD's hold (CydOptions::ResetGuard, 2 s) ---- */
#define RESET_HOLD_MS 2000UL

static void draw_reset_prompt(bool armed, bool holding, uint32_t held_ms, const char *note)
{
    int h = LCD_HEIGHT, row;
    refresh_line_h();
    row = line_h + 4;
    scr_begin();
    scr_text(2, 1, "Start a new egg?");
    scr_text(2, 2 * row, "Pet progress & eggs");
    scr_text(2, 3 * row, "will be lost forever.");
    scr_text(2, 4 * row, "Nothing can undo it.");
    scr_text(2, 6 * row, armed ? "  Cancel" : "> Cancel");
    scr_text(2, 7 * row, armed ? "> Yes, reset pet" : "  Yes, reset pet");
#ifdef HAVE_LCD_COLOR
    if (holding) {
        unsigned pct = held_ms >= RESET_HOLD_MS ? 100 : (unsigned)(held_ms * 100UL / RESET_HOLD_MS);
        fr(0x4208, 2, 8 * row + 2, 200, 8);
        fr(0xFD20, 2, 8 * row + 2, (int)(2 * pct), 8);
    }
#else
    (void)holding; (void)held_ms;
#endif
    if (note[0]) scr_text(2, 9 * row, note);
    scr_text(2, h - line_h - 2, "Scroll: choose  MENU: cancel");
    scr_end();
}

/* true = reset confirmed */
static bool reset_prompt(void)
{
    bool armed = false, holding = false, was_sel = false;
    uint32_t hold_at = 0;
    const char *note = "";
    while (!usb_pending) {
        uint32_t now = now_ms();
        int action;
        bool sel = false;
#ifdef BUTTON_SELECT
        sel = (rb->button_status() & BUTTON_SELECT) != 0;
#endif
        if (!armed || !sel) holding = false;
        else if (!holding && !was_sel) { holding = true; hold_at = now; }
        was_sel = sel;
        if (holding && now - hold_at >= RESET_HOLD_MS) return true;

        draw_reset_prompt(armed, holding, holding ? now - hold_at : 0, note);
        action = rb->get_action(CONTEXT_STD, HZ / 20);
        switch (action) {
        case ACTION_STD_CANCEL:
        case ACTION_STD_MENU:
            return false;
        case ACTION_STD_NEXT:
        case ACTION_STD_PREV:
            armed = !armed; holding = false; note = "";
            break;
        case ACTION_STD_OK:
            if (!armed) return false;                       /* Cancel is selected: close the prompt */
            note = "Hold SELECT for 2 seconds";
            break;
        default:
            if (rb->default_event_handler(action) == SYS_USB_CONNECTED) usb_pending = true;
            break;
        }
    }
    return false;
}

/* Remove the pet (and its egg) and its file; the Adopt page opens next */
static void pet_reset(void)
{
    rb->memset(&pet, 0, sizeof(pet));
    mystery_body = false;
    mv.init = false;
    pet_save();                       /* no pet: removes the file */
}

static void rename_pet(void)
{
    char name[NAME_MAX_CHARS + 1];
    if (!pet.exists) { rb->splash(HZ * 2, "No pet to rename"); return; }
    rb->strlcpy(name, pet.name, sizeof(name));
    if (!ask_name(name)) return;
    rb->strlcpy(pet.name, name, sizeof(pet.name));
    pet_dirty = true;
    pet_save();
    rb->splash(HZ * 2, name[0] ? "Name saved" : "Name cleared");
}

/* ---- Options: a list screen (SELECT toggles or opens, CANCEL goes back) ---- */
enum { OPT_SECONDS, OPT_BACKLIGHT, OPT_SLEEP, OPT_WAKE, OPT_CHARGE, OPT_SPEED, OPT_MUSIC, OPT_RENAME, OPT_RESET, OPT_COUNT };
static const char *const speed_names[3] = { "Smooth", "Simulation", "Efficient 1 FPS" };

/* RockBox's own Yes/No strings, so the options follow the firmware language */
static const char *yes_no(bool v) { return rb->str(v ? LANG_SET_BOOL_YES : LANG_SET_BOOL_NO); }

static const char *options_row(int item, void *data, char *buf, size_t len)
{
    (void)data;
    switch (item) {
    case OPT_SECONDS:   rb->snprintf(buf, len, "Show seconds: %s", yes_no(show_seconds)); break;
    case OPT_BACKLIGHT: rb->snprintf(buf, len, "Keep backlight on USB: %s", yes_no(keep_backlight_on_usb)); break;
    case OPT_SLEEP:     rb->snprintf(buf, len, "Sleep time: %02d:00%s", sleep_hour, sleep_hour == wake_hour ? " (off)" : ""); break;
    case OPT_WAKE:      rb->snprintf(buf, len, "Wake time: %02d:00", wake_hour); break;
    case OPT_CHARGE:    rb->snprintf(buf, len, "Rest while charging: %s", yes_no(rest_while_charging)); break;
    case OPT_SPEED:     rb->snprintf(buf, len, "Speed: %s", speed_names[render_speed]); break;
    case OPT_MUSIC:     rb->snprintf(buf, len, "Music reactive: %s", yes_no(music_reactive)); break;
    case OPT_RENAME:    return "Change pet name";
    default:            return "Reset pet";
    }
    return buf;
}

/* RockBox's own number picker, 00:00-23:00 in whole hours */
static const char *hour_text(char *buf, size_t len, int hour, const char *unit)
{
    (void)unit;
    rb->snprintf(buf, len, "%02d:00", hour);
    return buf;
}

static void set_hour(const char *label, int *hour)
{
    rb->set_int((const unsigned char *)label, "", UNIT_INT, hour, NULL, 1, 0, 23, hour_text);
}

static void options_menu(void)
{
    int sel = 0;
    while (!usb_pending) {
        int r = pick_list("Options", OPT_COUNT, options_row, &sel);
        if (r < 0) break;
        switch (r) {
        case OPT_SECONDS:   show_seconds = !show_seconds; break;
        case OPT_BACKLIGHT: keep_backlight_on_usb = !keep_backlight_on_usb; break;
        case OPT_SLEEP:     set_hour("Sleep time", &sleep_hour); break;
        case OPT_WAKE:      set_hour("Wake time", &wake_hour); break;
        case OPT_CHARGE:    rest_while_charging = !rest_while_charging; break;
        case OPT_SPEED:     render_speed = (render_speed + 1) % 3; break;
        case OPT_MUSIC:     music_reactive = !music_reactive; break;
        case OPT_RENAME:    rename_pet(); break;
        default:
            if (!pet.exists) { rb->splash(HZ * 2, "No pet to reset"); break; }
            if (reset_prompt()) {
                pet_reset();
                configfile_save(CONFIG_FILE_NAME, config, CONFIG_COUNT, 0);
                return;                         /* the main loop opens Adopt a pet */
            }
            break;
        }
        configfile_save(CONFIG_FILE_NAME, config, CONFIG_COUNT, 0);
    }
    configfile_save(CONFIG_FILE_NAME, config, CONFIG_COUNT, 0);
}

static void show_info(void)
{
    char age[16];
    struct env e;
    int stage = stage_for_age(pet.age_min);
    env_read(&e);
    if (!pet.exists) { rb->splash(HZ * 2, "No pet yet"); return; }
    fmt_age(age, sizeof(age), pet.age_min);
    if (pet.name[0]) rb->splashf(HZ, "%s", pet.name);
    {
        char track[96];
        if (track_line(track, sizeof(track))) rb->splashf(HZ * 2, "Playing: %s", track);
    }
    if (pet_is_egg())
        rb->splashf(HZ * 3, "Egg, generation %u. It hatches in %lu min.", (unsigned)pet.generation,
                    (unsigned long)(EGG_MINUTES - pet.age_min));
    else
        rb->splashf(HZ * 4, "%s, %s, generation %u. Age %s. Happy %d. Waste %d. Meal in %lu min. %s%s",
                    sp_label(pet.species), sp_stage_name(pet.species, stage), (unsigned)pet.generation, age,
                    pet.happy, pet.waste, (unsigned long)((pet.meal_cd_ms + 59999UL) / 60000UL),
                    pet.has_egg ? "Egg laid. " : "", status_text(&e));
}

/* Returns true to quit the plugin; *usb is set when the menu reported USB. */
static bool care_menu(bool *usb)
{
    static int sel = 0;
    int r;
    MENUITEM_STRINGLIST(menu, "ClickGotchi", NULL,
                        "Resume", "Feed", "Clean up", "Fertilise (lay an egg)", "Pet info",
                        "Options", ID2P(LANG_MENU_QUIT));
    r = rb->do_menu(&menu, &sel, NULL, false);
/* a USB plug during the menu makes do_menu return a negative code (MENU_ATTACHED_USB) and eats the event */
    if (r < 0 && rb->usb_inserted()) { *usb = true; return true; }
    switch (r) {
    case 1: rb->splash(HZ * 2, pet_feed()); break;
    case 2: rb->splash(HZ * 2, pet_clean()); break;
    case 3:
        if (fertilise_blocked() == NULL && !rb->yesno_pop("Lay an egg now?"))
            break;
        rb->splash(HZ * 3, pet_fertilise());
        break;
    case 4: show_info(); break;
    case 5:
        options_menu();
        if (usb_pending) { *usb = true; return true; }
        break;
    case 6: return true;
    default: break;
    }
    pet_save();
    return false;
}

/* Returns PLUGIN_OK to quit, or PLUGIN_USB_CONNECTED when USB is plugged in */
static enum plugin_status main_loop(void)
{
    int action;
    uint32_t last = now_ms();
    bool usb_from_menu = false;

    while (true) {
        struct env e;
        uint32_t now;

        if (!pet.exists) {                      /* no pet (first run, or after Reset pet): Adopt a pet */
            if (!adopt_flow())
                return usb_pending ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
            last = now_ms();
        }
        now = now_ms();

        env_read(&e);
        /* time passes whether or not the screen is drawn; asleep or charging = resting */
        pet_tick(now - last, e.asleep || e.resting_charge);
        last = now;

        draw_screen();

        if (keep_backlight_on_usb && e.usb)
            rb->backlight_on();

        if (pet_dirty && (uint32_t)(now - pet_last_save) >= 60000UL)
            pet_save();
        else if (age_since_save >= 15)
            pet_save();

        /* Home screen keys use the music (WPS) context: play/pause, skip, volume, MENU, and click-hold SELECT to clean */
        action = rb->get_action(CONTEXT_WPS, render_speed == 0 ? HZ / 30 : render_speed == 1 ? HZ / 10 : HZ);

        switch (action) {
        case ACTION_WPS_PLAY: {
            int st = rb->audio_status();
            if (st & AUDIO_STATUS_PLAY) rb->audio_pause();
            else if (st & AUDIO_STATUS_PAUSE) rb->audio_resume();
            break;
        }
        case ACTION_WPS_SKIPNEXT: rb->audio_next(); break;
        case ACTION_WPS_SKIPPREV: rb->audio_prev(); break;
        case ACTION_WPS_VOLUP:    rb->adjust_volume(1); break;
        case ACTION_WPS_VOLDOWN:  rb->adjust_volume(-1); break;
        case ACTION_WPS_CONTEXT:  /* SELECT held: clean the environment (once per hold, not on every repeat) */
            if ((uint32_t)(now_ms() - clean_hold_ms) > 3000UL) {
                clean_hold_ms = now_ms();
                rb->splash(HZ * 2, pet_clean());
                last = now_ms();
            }
            break;
        case ACTION_WPS_QUICKSCREEN:   /* MENU held: leave ClickGotchi */
            return PLUGIN_OK;

        case ACTION_STD_CANCEL:
            return PLUGIN_OK;

        case ACTION_WPS_MENU:
            if (care_menu(&usb_from_menu))
                return usb_from_menu ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
            last = now_ms();
            break;

        default:
            break;
        }

        if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
            return PLUGIN_USB_CONNECTED;
    }
}

enum plugin_status plugin_start(const void *parameter)
{
    enum plugin_status status;

    (void)parameter;

    configfile_load(CONFIG_FILE_NAME, config, CONFIG_COUNT, 0);
    state_load();
    boot_count++;
    state_save();

    rng_state = mix32(now_ms() + (uint32_t)rb->mktime(rb->get_time()));
    if (!rng_state) rng_state = 1;

    if (!pet_load())
        rb->memset(&pet, 0, sizeof(pet));       /* no pet yet: the main loop opens "Adopt a pet" */
    pet_last_save = now_ms();

    rb->lcd_clear_display();
    status = main_loop();

    pet_save();
    configfile_save(CONFIG_FILE_NAME, config, CONFIG_COUNT, 0);
    return status;
}