#include "longshot.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace longshot {

namespace {

// Offsets into game.dll.
constexpr uintptr_t kImpactTable = 0xd68b8;      // what a hit on each surface throws up, from surface 3
constexpr uintptr_t kNoImpact = 0xd68a4;
constexpr uintptr_t kDirtImpact = 0xd67e3;
constexpr uintptr_t kMostAnimalsCheck = 0xbe5aa;  // inside the cmp of the generator's limit
constexpr uintptr_t kTrimTotal = 0xbdbb3;         // inside the mov of what it trims down to
constexpr uintptr_t kTrimScale = 0xbdbae;         // inside the fdivr of the same again, as a float
constexpr uintptr_t kFortyFloat = 0x19ff60;
constexpr uintptr_t kGroupRollCall = 0xbe946;     // inside the call that rolls a group's size
constexpr uintptr_t kRandom = 0x19a2f0;
constexpr uintptr_t kTickSlots[] = {0x1a4ca4, 0x1a561c, 0x1a6184};  // every kind of animal has the same tick
constexpr uintptr_t kMusicSlot = 0x19b0c8;      // the hunt's method that plays a sting of music
constexpr uintptr_t kFireCall = 0xa75fb;
constexpr uintptr_t kFire = 0x10290;
constexpr uintptr_t kSensesCall = 0xb6868;
constexpr uintptr_t kBulletRayCall = 0xd4a83;   // a bullet's flight, not the rangefinder's
constexpr uintptr_t kRayCast = 0xd4430;
constexpr uintptr_t kWaterTargetCall = 0xb1c8a;  // a thirsty animal's
constexpr uintptr_t kFleeTargetCalls[] = {0xb201b, 0xb20b9};
constexpr uintptr_t kFleeTarget = 0xb1d90;
constexpr uintptr_t kAreaTarget = 0xbb3d0;
constexpr uintptr_t kSenses = 0xb59c0;
constexpr uintptr_t kAnimalManager = 0xbab80;
constexpr uintptr_t kGetValue = 0x19aaa8;
constexpr uintptr_t kSetValue = 0x19aa68;
constexpr uintptr_t kGetPoint = 0x19a90c;
constexpr uintptr_t kSetPoint = 0x19aa5c;
constexpr uintptr_t kFindInPool = 0x19ab0c;
constexpr size_t kPoolEntries = 0x50;
constexpr size_t kEntryNext = 4;
constexpr size_t kEntryObject = 8;
constexpr size_t kObjectChildren = 0xac;
constexpr size_t kManagerAnimals = 4;
constexpr size_t kManagerSenses = 0x10;         // how much the hunter can be sensed
constexpr size_t kSensesSmell = 0x10;
constexpr size_t kSensesSight = 0x14;
constexpr size_t kSensesHearing = 0x18;
constexpr size_t kManagerCount = 8;
constexpr size_t kManagerBirds = 0x30;
constexpr size_t kManagerAreas = 0x8c;
constexpr size_t kManagerAreaCount = 0x90;
constexpr size_t kAreaPosition = 8;
constexpr size_t kAreaResource = 0x2c;
constexpr size_t kUnitFlags = 6;
constexpr unsigned char kUnitAlive = 0x80;
constexpr size_t kAnimalNameText = 0x160;
constexpr size_t kAnimalPosition = 0x338;
constexpr size_t kAnimalMate = 0x43c;
constexpr size_t kAnimalLurePlace = 0x458;
constexpr size_t kAnimalHurt = 0xbe6;
constexpr size_t kAnimalDead = 0xbe7;
constexpr size_t kBrainAnimal = 0x23c;
constexpr size_t kBrainHunterFear = 0xdc;
constexpr size_t kBrainEnemyFear = 0xe0;
constexpr size_t kBrainPrey = 0xe4;
constexpr size_t kBrainHunger = 0x110;
constexpr size_t kBrainThirst = 0x114;
constexpr size_t kBrainTiredness = 0x118;
constexpr size_t kBrainLure = 0x108;
constexpr size_t kBrainAggression = 0xb8;
constexpr size_t kBrainSight = 0x130;
constexpr size_t kBrainHearing = 0x138;
constexpr size_t kBrainGoal = 0x244;
constexpr size_t kBrainGoalStage = 0x248;
constexpr size_t kBirdChances = 0x58;
constexpr size_t kBirdChanceCount = 0x5c;
constexpr size_t kBirdPeriod = 0x64;
constexpr size_t kBirdTimer = 0x68;
constexpr size_t kBirdMost = 0x80;
constexpr size_t kMethodPosition = 0x3c;
constexpr size_t kHitsFirst = 4;
constexpr size_t kHitsCount = 8;
constexpr size_t kHitSize = 0x20;
constexpr size_t kHitGeom = 0x1c;
constexpr size_t kGeomKind = 0x6c;
constexpr int kWaterGeom = 1;
constexpr size_t kAnimalTarget = 0x474;     // where it's walking to
constexpr size_t kTargetNear = 4;           // how close counts as there
constexpr size_t kTargetPlace = 0x10;

// A thirsty animal is sent to the middle of the water. It stops where it's no deeper than this,
// on the way there from where it is.
constexpr float kDeepestDrink = 0.4f;
constexpr float kShoreStep = 1.0f;
constexpr float kShoreSearch = 200.0f;
constexpr float kAtTheEdge = 1.5f;
constexpr float kLookDownFrom = 10.0f;

// An animal's tick takes how much time has passed, so one that's far off can run less often and
// catch up. Close up, and anywhere when looked at through a scope, they run smoothly.
constexpr float kEveryFrame = 130.0f;
constexpr float kNearLater = 300.0f;
constexpr float kFarLater = 600.0f;
constexpr float kNearGap = 1.0f / 30.0f;
constexpr float kFarGap = 1.0f / 15.0f;
constexpr float kFurthestGap = 1.0f / 8.0f;
constexpr float kInScope = 0.9f;        // the cosine of how far off the view a scope can see
constexpr double kReportSeconds = 30.0;

// The chase music is only for when something's coming after you. The game also plays one as any
// hunter starts on any animal, however far away, and another as a target comes within 200 m, seen
// or not. With this many animals, that can be the moment a free hunt starts.
constexpr int kTargetNearMusic = 0;
constexpr int kAnimalAttacksMusic = 1;
// Anything aggressive that's a little afraid of you can come for you, from as far as it can hear,
// so this waits until there's something there to run from.
constexpr int kHunterAttackedMusic = 2;
constexpr int kAttackHunterGoal = 13;
constexpr float kChaseMusicReach = 80.0f;
constexpr float kChaseMusicVolume = 1.0f;

constexpr int kFirstImpactSurface = 3;
constexpr int kTreeSurface = 8;
constexpr int kWaterResource = 1;
constexpr uint32_t kGameMostAnimals = 40;

// The check that limits the generator only has room for a byte.
constexpr uint32_t kMostAnimals = 127;
const float kMostAnimalsFloat = static_cast<float>(kMostAnimals);
constexpr float kGroupShare = 0.95f;      // a group is 0.05 to 1 of maximal_herd_size, rounded down
constexpr float kLeastGroupShare = 0.05f;
constexpr int kMostInAnyHerd = 15;

constexpr float kSameGroup = 24.0f;       // the game puts a group on a circle of 10 m
constexpr float kCloseGroup = 6.0f;       // how far out a small group spreads
constexpr float kSpreadPerAnimal = 1.0f;
constexpr float kGameGroupRadius = 10.0f;
constexpr int kAlwaysClose = 4;
constexpr int kMostYoung = 2;
constexpr char kYoungHorns[] = "simple_beam_part0";  // young have fewer shapes of horn, but always this one

constexpr float kWantsDrink = 0.95f;       // beats anything but real danger
constexpr float kNearWater = 220.0f;
constexpr double kDrinkEvery = 360.0;
constexpr double kDrinkFor = 180.0;
constexpr double kPeakDrinkFor = 240.0;    // dawn and dusk
constexpr double kQuietDrinkEvery = 480.0;  // the middle of the day
constexpr double kQuietDrinkFor = 120.0;
constexpr double kWanderSeconds = 150.0;   // a hunter has had its drink, and isn't hunting yet
constexpr double kWaterShySeconds = 300.0;  // after being frightened while it was going to drink
constexpr double kProvokedSeconds = 90.0;
constexpr float kTooClose = 15.0f;
constexpr float kShotReach = 60.0f;
constexpr double kShotSeconds = 1.5;

constexpr int kDawnFrom = 5, kDawnTo = 9;
constexpr int kDuskFrom = 16, kDuskTo = 20;
constexpr int kQuietFrom = 11, kQuietTo = 14;
constexpr int kNightFrom = 21, kNightTo = 5;
constexpr float kPeakHunger = 1.4f;
constexpr float kPeakRest = 0.5f;
constexpr float kQuietHunger = 0.6f;
constexpr float kQuietRest = 2.0f;
constexpr float kNightHunger = 0.5f;        // grazers settle down for the night
constexpr float kNightRest = 2.5f;
constexpr float kProwlHunger = 1.6f;        // and hunters are at their most active
constexpr float kProwlRest = 0.5f;
constexpr unsigned kNightBirds = 3;
constexpr unsigned kDayBirds = 24;

constexpr int kAlarms = 8;
constexpr float kAlarmFear = 0.35f;       // calmer than this isn't worth running from
constexpr float kAlarmFade = 0.75f;       // each animal that takes it up passes on less
constexpr float kAlarmReach = 60.0f;
constexpr double kAlarmSeconds = 2.0;
constexpr double kAlarmGap = 1.0;
constexpr float kDeathAlarm = 1.0f;       // one of them being shot or killed
// Anything aggressive that's frightened enough can come for the hunter, from wherever it is. Past
// this it runs instead.
constexpr float kChargeReach = 100.0f;

// The game sends a frightened animal 50 to 100 m off, up to 45 degrees either side of straight away,
// and picks again each time it gets there or is stuck, so it zigzags and circles, sometimes towards
// you. Instead a herd keeps to one line, straight away if it can, better uphill, never into water.
constexpr int kFleeGoal = 11;
constexpr int kGoalDone = 3;              // a goal's stage once it's over, so another's picked
constexpr int kFleeAngles = 9;            // straight away, then each side in turn
constexpr float kFleeAngleStep = 0.35f;   // about 20 degrees, so up to 80 either side
constexpr float kFleeLook = 120.0f;       // how far a line has to stay out of the water
// The game's own places are 50 to 100 m off. Past 110 m, the distance set as running away starts,
// its movement doesn't make a path, and asks for a new place every update, which is going round in
// circles, and slow.
constexpr float kFleeStep = 90.0f;
constexpr float kHillPull = 0.35f;        // what climbing's worth against running straight away
constexpr float kHillScale = 30.0f;       // a climb this big counts in full
constexpr float kLookFromAbove = 60.0f;
// A place to go to is on the ground there. Running away it only counts as reached within 20 m, and
// walking within 1.1 m, both counted up and down too (`+0xc125b`, `+0xc11ec`). A place left at the
// height it started from could be 20 m up in the air or under a hillside, which can't be reached, and
// it ran round and round under it.
constexpr float kGroundFromAbove = 120.0f;
constexpr size_t kAnimalGoalPlace = 0x49c;
constexpr double kEscapeKeeps = 25.0;
constexpr float kThreatMoved = 40.0f;     // a new line if what it's running from has moved this far
// With nowhere else to go, an animal that can fight does when you're close, and one that can't runs
// straight away regardless, into the water if it has to. Neither runs at you to get away.
constexpr float kCornerReach = 40.0f;
constexpr double kCorneredSeconds = 10.0;

// At night grazers go to a dry spot near the herd, higher up and away from you, and lie down. It
// takes half a minute to fall asleep, and then they see nothing and hear a third as well. One in
// each herd keeps watch.
constexpr int kRestGoal = 6;
constexpr float kBedRings[] = {25.0f, 50.0f};
constexpr int kBedSpokes = 8;
constexpr float kDriestBed = 0.05f;
constexpr float kBedAway = 0.5f;          // what being further from you is worth against height
constexpr float kBedArrive = 5.0f;
constexpr float kBedSpread = 4.0f;        // how far apart a herd lies
constexpr float kBedTired = 0.9f;
constexpr double kFallAsleepSeconds = 30.0;
constexpr float kSleepHearing = 0.35f;
constexpr float kAwake = 0.3f;            // with less sleep than this it's up and about
constexpr float kNoiseWakes = 0.6f;       // a second, by how loud you are to it
constexpr float kShotWakes = 1.6f;
constexpr float kShotWakeReach = 400.0f;
constexpr float kNearMiss = 8.0f;
constexpr float kNearMissFear = 0.8f;
constexpr double kBulletSeconds = 2.0;
constexpr double kSentinelSeconds = 10.0;
constexpr double kLongestThought = 1.0;

// A wounded animal runs, then lies up, and runs on again if you come after it.
constexpr float kWoundRunShortest = 150.0f;
constexpr float kWoundRunLongest = 300.0f;
constexpr float kWoundFear = 1.0f;
constexpr float kFlushReach = 45.0f;
constexpr float kFlushHears = 0.3f;
constexpr float kFlushRunsOn = 120.0f;

// A bear or cougar with young goes for you if you get close to them, or to her.
constexpr float kDefendReach = 40.0f;
constexpr float kDefendFear = 0.9f;        // over 0.8, so danger is what she does
constexpr double kYoungSeenSeconds = 5.0;

// Something running into you knocks the view.
constexpr float kBumpReach = 1.6f;
constexpr float kBumpHeight = 2.0f;
constexpr float kBumpSpeed = 2.0f;        // m/s towards you
constexpr float kHardBump = 8.0f;
constexpr float kSoftestBump = 0.3f;
constexpr double kBumpGap = 1.5;
constexpr double kLongestStep = 0.25;     // between two looks at where it is, for its speed
constexpr int kBullets = 64;

// Calls. Each animal has one sound of its own, which the game only plays when it answers a call you
// make, and only within 200 m. Here it calls in alarm, when it's hit, as a bear comes for you, now
// and then to keep in touch, more at dawn and dusk for the males of some kinds, and wolves howl
// together at night. A call carries to 400 m.
constexpr uintptr_t kPlayCall = 0xae1b0;
constexpr uintptr_t kPlayAnimation = 0xb3680;
constexpr uintptr_t kCallHeardWithin = 0xae32b;  // inside the fcomp of how close it has to be to call
constexpr uintptr_t kCallFadesBy = 0xb04f3;      // inside the push of how far its call is heard
constexpr uintptr_t kTwoHundredSquared = 0x1a2044;
constexpr size_t kAnimalSounds = 0x31c;
constexpr size_t kAnimalSoundCount = 0x320;
constexpr size_t kAnimalAnimations = 0x75c;
constexpr size_t kBrainNeed = 0x240;
constexpr unsigned kCallSound = 12;
constexpr int kCallAnimation = 0x28;
constexpr int kDangerNeed = 6;
constexpr float kGameCallReach = 200.0f;
constexpr float kCallReach = 400.0f;
const float kCallReachSquared = kCallReach * kCallReach;
constexpr double kHerdAlarmGap = 30.0;    // a herd's alarm call, once
constexpr double kGrowlGap = 20.0;
constexpr double kHowlEvery = 240.0;
constexpr double kHowlJoins = 4.0;        // the rest of the pack join in within this long
constexpr float kDawnCalls = 4.0f;        // how many times as often
constexpr char kWolfHowl[] = "/sounds/ambient/amb_wolf1.ogg";

// Running. An animal moves by its animation, as far as the root moved this tick, times its size,
// turned to face its way (`+0xc3fe7`). So its run is only as fast as the run animation, which for
// most is 3 to 6 m/s, and plays at a rate set as it's made. Running, that rate goes up, and the
// root's move with it past that, to make each kind as fast as it really is. Turning eases towards
// where it's going by the seconds passed each tick (`+0xc41c0`), which takes about a second to
// mostly come round, so that's quicker too, and most for the cats.
constexpr uintptr_t kRootMotionCall = 0xc3fe7;
constexpr uintptr_t kRootMotion = 0x173f00;
constexpr uintptr_t kTurnCall = 0xc41c2;           // inside the call through the import of slerp_generic
constexpr uintptr_t kSlerpImport = 0x19a164;
constexpr size_t kAnimalRotation = 0x348;
constexpr size_t kAnimalAnimationRate = 0x3b0;
constexpr size_t kAnimalMoveMode = 0x474;
constexpr size_t kAnimalThreat = 0x480;
constexpr size_t kControllerProcessor = 4;
constexpr int kAttackProcessor = 4;                 // of the names at +0x1e1560
constexpr int kRunAwayProcessor = 5;
constexpr int kFleeMode = 1;
constexpr int kEnemyGoal = 12;
constexpr int kHuntPreyGoal = 4;
constexpr float kMostLegs = 2.2f;                   // how much faster the legs go
constexpr float kMostStride = 1.6f;                 // and how much further each step takes it on top
constexpr float kWalkTurnShare = 0.4f;              // of the extra turning, when it isn't running
constexpr float kRecoverSeconds = 45.0f;            // from spent to fresh
constexpr float kTiredFrom = 0.4f;                  // stamina below this slows it
constexpr float kTiredPace = 0.6f;                  // to this share of its top speed, when spent
constexpr float kWoundedPace = 0.7f;
constexpr float kWoundedDrain = 1.6f;
constexpr float kYoungPace = 0.8f;                  // and a mother keeps to it
constexpr float kSpent = 0.05f;
constexpr double kGiveUpSeconds = 40.0;
// Jinking: cutting off to the side when what's after it gets close.
constexpr float kJinkReach = 25.0f;
constexpr float kJinkAngle = 1.2f;                  // about 70 degrees
constexpr float kJinkStep = 30.0f;
constexpr double kJinkShortest = 0.8;
constexpr double kJinkLongest = 1.4;
constexpr double kJinkGapShortest = 2.5;
constexpr double kJinkGapLongest = 4.5;
// A bluff stops short, or veers off, this far from you.
constexpr float kBluffNearest = 8.0f;
constexpr float kBluffFurthest = 20.0f;
constexpr double kBluffSeconds = 30.0;
constexpr float kBluffRunsFear = 0.9f;

// The attack goal (from +0xb25a9) only charges within 75 m (+0xb265c). Further off it walks, slower
// the closer it gets, a quarter of its pace at 75 m, then charges. Attacks start within 100 m here,
// so every one crept the first stretch. It charges from 150 m instead.
constexpr uintptr_t kChargeFromUse = 0xb265e;  // inside the fcomp of the 75
constexpr uintptr_t kGameChargeFrom = 0x1a4bd0;
const float kChargeFrom = 150.0f;
constexpr size_t kAnimalBrain = 0x4f8;

// Bears have no pose for drinking or grazing, so at the water they just stand. They drink for less.
constexpr const char* kNoDrinkingPose[] = {"grizzly", "blackbear"};
constexpr double kStandingDrinkFor = 40.0;

// Mountain goats live up high, on mountainsides. A herd starts, or arrives, on the highest ground near
// where it would have, with steepness counting for more, keeps within 70 m of that unless it's
// thirsty or frightened, drinks a third as often as anything else, and runs uphill.
constexpr const char* kClimbers[] = {"goat"};
constexpr float kClimbSettleReach = 200.0f;  // at the start, when where you'll be isn't known yet
constexpr float kClimbArriveReach = 250.0f;
constexpr float kClimbStep = 25.0f;
constexpr int kClimbSpokes = 16;
constexpr float kClimbLookFrom = 500.0f;     // above where it is, for the ground anywhere near
constexpr float kSlopeStep = 10.0f;
constexpr float kSteepWorth = 25.0f;         // m of height that a slope of 1 in 1 is worth
constexpr float kSteepest = 2.0f;
constexpr float kClimberRoam = 70.0f;
constexpr double kClimberDrinksLess = 3.0;
constexpr float kClimberHillPull = 1.2f;

// A hunter drinking, or wandering after, still notices you this close if it sees or hears you.
constexpr float kNoticeReach = 60.0f;

// A herd keeps one line, but one that would take an animal this much towards what it's running from
// isn't its line. It takes its own.
constexpr float kLineToward = 0.2f;

// What running looks like, for the log: full circles, and running at you without attacking.
constexpr double kTrackSeconds = 0.5;
constexpr double kTrackLost = 2.0;
constexpr double kCircleWithin = 20.0;
constexpr float kTrackSpeed = 2.0f;
constexpr float kFullCircle = 6.2831853f;
constexpr float kAtYouReach = 60.0f;
constexpr float kAtYou = 0.9f;             // the cosine of how straight at you
constexpr double kAtYouGap = 10.0;
constexpr const char* kProcessorNames[] = {"rest", "eat", "drink", "move", "attack", "run_away", "scrape", "injury"};

// Running away, some still go round and round a place they can't get to, without asking for another.
// Three quarters of a turn like that, and it's given a nearer one, off to one side, which it runs to
// the game's way (`+0xc1050`).
constexpr uintptr_t kRunTo = 0xc1050;
constexpr float kStuckTurn = 4.712389f;
constexpr float kUnstickAngle = 1.0f;          // about 60 degrees
constexpr float kUnstickStep = 40.0f;
// Senses only reach so far (`+0xb59c0`): sight 90 m (`+0xc2c17`), smell 300 m and hearing 250 m. What
// sets off an animal you're watching through binoculars is logged, with what it's running from.
constexpr int kMostSpookedNotes = 30;

// Blood. A wounded animal drips every 10 over its bleeding (+0xbf0), times 2 to 4, seconds
// (`+0xc16b0`), one small stain at a time (`+0xad0c0`), and one more where it's hit. Here it drips
// two and a half times as often, more at once the harder it bleeds, a hit throws up a spray, and a
// pool forms where it lies up or dies. Bleeding draws predators along the trail.
constexpr uintptr_t kDripCall = 0xc1732;
constexpr uintptr_t kHitBloodCall = 0xc6561;
constexpr uintptr_t kDropBlood = 0xad0c0;
constexpr uintptr_t kDripEveryUse = 0xc16e6;  // inside the fld of the 10
constexpr uintptr_t kTen = 0x1a5fe8;
constexpr size_t kAnimalBleeding = 0xbf0;
constexpr size_t kAnimalBleeds = 0xbe9;
const float kDripEvery = 4.0f;
constexpr float kDropsPerBleed = 1.0f;
constexpr int kMostDrops = 6;
constexpr int kHitDrops = 6;
constexpr int kHitStain = 2;
constexpr int kPoolStain = 1;
constexpr int kPoolDrops = 10;
constexpr int kPoolGrows = 2;
constexpr double kPoolGrowSeconds = 20.0;
constexpr int kBloodSpots = 32;
constexpr double kBloodSpotGap = 2.0;
constexpr double kBloodScentSeconds = 180.0;
constexpr float kBloodScent = 250.0f;
constexpr float kBloodFound = 25.0f;          // close enough that the game's own hunting takes over
constexpr float kBloodPull = 1.1f;

// Tracks. A footprint (`+0xacf00`) goes down every `spoor_dist`, on soft ground, through
// `+0xf3670`. Here it's stamped twice, so it's darker, and further apart when it's running. The
// hunter leaves prints too, with the game's own `hunter_trail`, which nothing ever used.
constexpr uintptr_t kStampCall = 0xad0a4;
constexpr uintptr_t kStamp = 0xf3670;
constexpr uintptr_t kPlacePrint = 0xacf00;
constexpr uintptr_t kCurrentHunt = 0x4f50;
constexpr uintptr_t kStickers = 0x124500;
constexpr uintptr_t kStickerNamed = 0x8efe0;
constexpr size_t kAnimalPrintGap = 0x2e8;
// All a footprint reads of what's leaving it.
constexpr size_t kFeetMoving = 0x288;
constexpr size_t kFeetGround = 0x290;
constexpr size_t kFeetFrom = 0x1c0;
constexpr size_t kFeetTo = 0x1cc;
constexpr size_t kFeetScale = 0x344;
constexpr size_t kFeetWay = 0x368;
constexpr size_t kFeetTrail = 0x2f0;
constexpr size_t kFeetSize = 0x400;
constexpr int kSoftGround = 4;
constexpr float kRunningPrintGap = 1.8f;
constexpr float kHunterStride = 0.8f;
constexpr float kHunterSideStep = 0.3f;
constexpr char kHunterTrail[] = "hunter_trail";

// The files: bigger stains and prints, more of them kept, prints closer together.
constexpr float kBloodSize = 1.6f;
constexpr float kPrintSize = 1.7f;
constexpr float kPrintGapShare = 0.6f;
constexpr char kGameBloodLimits[] = "limits:\"50:200:400\"";
constexpr char kBloodLimits[] = "limits:\"150:600:1200\"";
constexpr char kGamePrintLimits[] = "limits:\"300:300:600\"";
constexpr char kPrintLimits[] = "limits:\"900:900:1800\"";
// Blood was drawn blended over the ground, unlit, so it shone at night. Multiplied with what's
// under it instead, at twice, it's as dark as the ground is, and red.
constexpr char kOverBlood[] = "dry.tex.fog.nc.decal.over";
constexpr char kStainedBlood[] = "dry.tex.fog.nc.decal.mult";
constexpr char kGameMultiply[] = "dst : zero";
constexpr char kTwiceMultiply[] = "dst : src_color";
constexpr char kLongWolfHowl[] = "/sounds/ambient/amb_wolf2.ogg";

// At night an animal only sees what's close, or a bit further if the hunter's carrying a light.
constexpr float kNightSight = 18.0f;
constexpr float kLitNightSight = 30.0f;
constexpr float kDimmestLight = 0.05f;

// A light draws animals in at night, until they're close enough to see who's holding it.
constexpr float kGlowDraws = 100.0f;
constexpr float kBeamDraws = 150.0f;
constexpr float kBeamDrawsWithin = 0.9f;  // the cosine of how far off where it's pointing
constexpr float kLightPull = 1.2f;        // beats anything but real danger
constexpr float kLightScare = 1.0f;
constexpr float kCalm = 0.2f;             // anything more frightened won't come
constexpr float kSeesYou = 0.05f;
constexpr int kLureGoal = 3;
constexpr double kLureLasts = 10.0;       // after a beam has moved off it
constexpr double kLureWalkGap = 4.0;      // how often it checks where you've got to
constexpr double kLureRests = 120.0;      // before it'll fall for that again

constexpr int kBirdKinds = 8;
constexpr int kVulture = 6;               // in the order of ambient_animals.sii
constexpr float kVultureShare = 0.65f;
constexpr float kCalmPeriod = 1.5f;
constexpr float kStartledPeriod = 0.12f;
constexpr unsigned kStartledBirds = 10;
constexpr double kStartledSeconds = 1.5;
constexpr double kStartleGap = 8.0;
constexpr float kBirdsNoticeAlarm = 180.0f;
constexpr float kBigMoveSpeed = 7.0f;     // m/s, which on foot is only ever a vehicle
constexpr double kLookSeconds = 1.0;
constexpr double kCarcassSeconds = 300.0;
constexpr float kCarcassReach = 400.0f;
constexpr double kCarcassLookSeconds = 0.5;  // a death is also an alarm, so this can't wait long

struct Kind {
    const char* name;
    int low;
    int medium;
    int high;
    int largest;   // the most in one group
    float young;   // the share of a group that can be young
    bool hunts;    // goes after other animals
    bool antlers;  // so only females can be changed into young
    bool family;   // alone, or a mother with young
};

constexpr Kind kKinds[] = {
    {"wtail", 14, 24, 40, 15, 0.3f, false, true, false},
    {"mule", 14, 24, 40, 15, 0.3f, false, true, false},
    {"reddeer", 14, 24, 40, 15, 0.3f, false, true, false},
    {"elk", 14, 24, 40, 15, 0.3f, false, true, false},
    {"buffalo", 14, 24, 40, 15, 0.3f, false, true, false},
    {"pronghorn", 14, 24, 40, 15, 0.3f, false, true, false},
    {"kudu", 14, 24, 40, 15, 0.3f, false, true, false},
    {"goat", 10, 18, 30, 10, 0.3f, false, true, false},
    {"bighorn", 10, 18, 30, 10, 0.3f, false, true, false},
    {"turkey", 10, 18, 30, 12, 0.3f, false, false, false},
    {"elephant", 8, 14, 22, 12, 0.25f, false, true, false},
    {"boar", 8, 14, 22, 7, 0.5f, false, false, false},
    {"moose", 5, 9, 14, 3, 0.5f, false, true, false},
    {"wolf", 6, 10, 16, 6, 0.25f, true, false, false},
    {"lion", 6, 10, 16, 6, 0.25f, true, false, false},
    {"grizzly", 4, 7, 12, 3, 1.0f, true, false, true},
    {"blackbear", 4, 7, 12, 3, 1.0f, true, false, true},
    {"cougar", 4, 7, 12, 3, 1.0f, true, false, true},
};

enum class DayPart { Day, Peak, Quiet, Night };

struct Gait {
    const char* kind;
    float run;      // m/s its own run animation covers
    float top;      // m/s at a full sprint
    float turn;     // how many times the game's turning, running
    float stamina;  // seconds of full sprint
    bool agile;     // cuts away when something's closing
    float bluff;    // the chance a charge stops short
};

constexpr Gait kGaits[] = {
    {"wtail", 5.9f, 13.0f, 4.0f, 30.0f, true, 0.0f},
    {"mule", 5.0f, 12.0f, 4.0f, 30.0f, true, 0.0f},
    {"reddeer", 4.8f, 12.0f, 3.5f, 30.0f, true, 0.0f},
    {"elk", 4.8f, 12.0f, 3.0f, 35.0f, false, 0.0f},
    {"buffalo", 3.4f, 12.0f, 2.5f, 25.0f, false, 0.35f},
    {"pronghorn", 6.0f, 20.0f, 4.0f, 60.0f, true, 0.0f},
    {"kudu", 4.6f, 13.0f, 4.0f, 30.0f, true, 0.0f},
    {"goat", 3.3f, 8.0f, 5.0f, 20.0f, true, 0.0f},
    {"bighorn", 4.4f, 11.0f, 5.0f, 25.0f, true, 0.0f},
    {"turkey", 10.5f, 10.5f, 5.0f, 20.0f, true, 0.0f},
    {"elephant", 7.7f, 10.0f, 1.5f, 40.0f, false, 0.5f},
    {"boar", 4.5f, 11.0f, 4.0f, 20.0f, false, 0.0f},
    {"moose", 9.5f, 13.0f, 2.5f, 30.0f, false, 0.35f},
    {"wolf", 10.3f, 15.0f, 6.0f, 60.0f, false, 0.0f},
    {"lion", 8.8f, 18.0f, 6.0f, 15.0f, false, 0.0f},
    {"grizzly", 5.4f, 14.0f, 3.5f, 20.0f, false, 0.4f},
    {"blackbear", 3.5f, 12.0f, 3.5f, 20.0f, false, 0.4f},
    {"cougar", 6.9f, 17.0f, 8.0f, 12.0f, false, 0.0f},
};

const Gait* gait_of(const Kind* kind) {
    if (!kind) return nullptr;
    for (const Gait& gait : kGaits) {
        if (!strcmp(gait.kind, kind->name)) return &gait;
    }
    return nullptr;
}

bool young_name(const char* name) { return name && (strstr(name, "cub") || strstr(name, "pup")); }

// How much faster the legs go, and how much further each step goes on top, for a speed.
void pace_shares(float run, float speed, float* legs, float* stride) {
    const float wanted = run > 0.0f && speed > run ? speed / run : 1.0f;
    *legs = wanted < kMostLegs ? wanted : kMostLegs;
    const float rest = wanted / *legs;
    *stride = rest < kMostStride ? rest : kMostStride;
}

// Full speed until it's tiring, then down to a share of it as it's spent.
float tired_share(float stamina) {
    if (stamina >= kTiredFrom) return 1.0f;
    const float left = stamina > 0.0f ? stamina / kTiredFrom : 0.0f;
    return kTiredPace + (1.0f - kTiredPace) * left;
}

struct Voice {
    const char* kind;
    float every;  // seconds between calls just to keep in touch, about, or 0 for none
    bool rut;     // males call far more at dawn and dusk
    bool alarm;   // calls when it panics
    bool growl;   // calls as it comes for you
    bool howl;    // howls with its pack at night
};

// Boars, cougars, lions, kudu and elephants have no sound of their own.
constexpr Voice kVoices[] = {
    {"wtail", 900.0f, false, true, false, false},
    {"mule", 900.0f, false, true, false, false},
    {"reddeer", 360.0f, true, true, false, false},
    {"elk", 360.0f, true, true, false, false},
    {"buffalo", 120.0f, false, true, false, false},
    {"pronghorn", 600.0f, false, true, false, false},
    {"goat", 600.0f, false, true, false, false},
    {"bighorn", 600.0f, false, true, false, false},
    {"turkey", 240.0f, true, true, false, false},
    {"moose", 600.0f, true, true, false, false},
    {"grizzly", 0.0f, false, false, true, false},
    {"blackbear", 0.0f, false, false, true, false},
    {"wolf", 0.0f, false, false, false, true},
};

const Voice* voice_of(const Kind* kind) {
    if (!kind) return nullptr;
    for (const Voice& voice : kVoices) {
        if (!strcmp(voice.kind, kind->name)) return &voice;
    }
    return nullptr;
}

bool male_name(const char* name) {
    if (!name || strstr(name, "female")) return false;
    for (const char* word : {"buck", "bull", "boar", "gobbler", "tom", "male"}) {
        if (strstr(name, word)) return true;
    }
    return false;
}

struct Wild {
    const Kind* kind = nullptr;
    float phase = 0.0f;
    float waiting = 0.0f;
    double peace_until = 0.0;
    double provoked_until = 0.0;
    double next_alarm = 0.0;
    double lured_until = 0.0;
    double lure_rests_until = 0.0;
    double next_lure_walk = 0.0;
    bool chase_music_held = false;
    bool hurt_seen = false;
    double water_shy_until = 0.0;
    double last_think = 0.0;
    double cornered_until = 0.0;
    float last_place[3] = {};
    double last_place_time = 0.0;
    double next_bump = 0.0;
    bool has_bed = false;
    bool bedded = false;
    float bed[3] = {};
    float sleep = 0.0f;
    double last_shot_heard = 0.0;
    double last_bullet_seen = 0.0;
    double follower_seen = 0.0;
    double young_seen = 0.0;
    float young_place[3] = {};
    float wound_run = 0.0f;
    bool lying_up = false;
    const Voice* voice = nullptr;
    bool male = false;
    double next_call = 0.0;
    double herd_alarm_at = -1000.0;
    double growled_at = -1000.0;
    double howled_at = 0.0;
    double joined_howl = 0.0;
    const Gait* gait = nullptr;
    bool young = false;
    float stamina = 1.0f;
    bool running = false;
    double gave_up_until = 0.0;
    double jink_until = 0.0;
    double next_jink = 0.0;
    float jink_side = 1.0f;
    bool jinking = false;
    bool charging = false;
    bool bluffing = false;
    float bluff_at = 0.0f;
    double bluffed_until = 0.0;
    bool bluff_veers = false;
    bool was_lying_up = false;
    double next_pool = 0.0;
    double next_blood_mark = 0.0;
    bool on_blood = false;
    float track_place[3] = {};
    double track_time = 0.0;
    float track_heading = 0.0f;
    bool has_heading = false;
    float turned = 0.0f;
    double turning_since = 0.0;
    double at_you_until = 0.0;
    unsigned places_given = 0;
    unsigned places_at_turn = 0;
    bool has_home = false;
    float home[3] = {};
    bool going_home = false;
    float unstick_side = 1.0f;
    bool was_running_away = false;
};

struct Escape {
    float heading[2];
    float threat[3];
    double until;
    bool cornered;
};

struct Bullet {
    float from[3];
    float to[3];
    double time;
};

struct BloodSpot {
    float place[3];
    double time;
};

struct Alarm {
    float place[3];
    float strength;
    double time;
};

struct Carcass {
    char* animal;
    float place[3];
    double seen;
};

using RandomFn = float(__fastcall*)(void*, void*);
using FireFn = void(__fastcall*)(void*, void*, void*, void*, void*, void*, void*);
using TickFn = void(__fastcall*)(char*, void*, float);
using MusicFn = void(__fastcall*)(void*, void*, int, float);
using SensesFn = void(__fastcall*)(char*, void*);
using RayCastFn = unsigned(__fastcall*)(void*, void*, int, const float*, const float*, float, char*);
using FleeTargetFn = float(__fastcall*)(char*, void*, const float*, float*);
using PlayCallFn = void(__fastcall*)(char*, void*);
using RootMotionFn = void(__fastcall*)(void*, void*, float*, float*);
using SlerpFn = void(__fastcall*)(float*, void*, const float*, const float*, float);
using ThreatPlaceFn = const float*(__fastcall*)(void*, void*, float*);
using DropBloodFn = void(__fastcall*)(char*, void*, int);
using RunToFn = void(__fastcall*)(char*, void*, bool);
using StampFn = bool(__fastcall*)(void*, void*, int, int, float*, float*, float*, float, int);
using PlacePrintFn = void(__fastcall*)(char*, void*);
using CurrentHuntFn = char*(__cdecl*)();
using StickersFn = char*(__fastcall*)(char*, void*);
using StickerNamedFn = int(__fastcall*)(char*, void*, const char* const*);
using PlayAnimationFn = void(__fastcall*)(char*, void*, int, int, int);
using AreaTargetFn = void(__fastcall*)(void*, void*, int, char*);
using ManagerFn = char*(__cdecl*)();
using GetValueFn = const char*(__fastcall*)(void*, void*, const char*, unsigned);
using SetValueFn = void(__fastcall*)(void*, void*, const char*, const char*, unsigned);
using GetPointFn = float*(__fastcall*)(void*, void*, float*, const char*, unsigned);
using SetPointFn = void(__fastcall*)(void*, void*, const char*, const float*, unsigned);
using FindInPoolFn = char*(__fastcall*)(void*, void*, const char*, unsigned, bool);
using PositionFn = const float*(__fastcall*)(void*, void*);

char* g_game = nullptr;
void* g_group_roll = nullptr;
bool g_whole_herd = false;

bool g_on_hunt = false;
bool g_active = false;
bool g_areas_read = false;
int g_hour = 8;
double g_now = 0.0;
float g_hunter[3] = {};
TickFn g_real_tick = nullptr;
MusicFn g_real_music = nullptr;
void* g_music_hunt = nullptr;
char* g_thinking = nullptr;
unsigned g_music_kept_quiet = 0;
bool g_zoomed = false;
float g_forward[3] = {};
bool g_lights_draw = false;
bool g_beam_read = false;
unsigned g_drawn = 0;
unsigned g_led_to_shore = 0;
unsigned g_lines_picked = 0;
unsigned g_cornered = 0;
unsigned g_barged = 0;
unsigned g_calls = 0;
float g_stride = 1.0f;
void* g_turn_slot = nullptr;
SlerpFn g_real_slerp = nullptr;
unsigned g_jinks = 0;
unsigned g_bluffs = 0;
unsigned g_gave_up = 0;
float g_fastest = 0.0f;
const char* g_fastest_kind = nullptr;
unsigned g_drops = 0;
unsigned g_circled = 0;
unsigned g_climbed = 0;
unsigned g_unstuck = 0;
int g_spooked_notes = 0;
float g_climbed_most = 0.0f;
unsigned g_gone_home = 0;
unsigned g_at_you = 0;
unsigned g_own_lines = 0;
char g_circle_note[320] = {};
char g_at_you_note[320] = {};
unsigned g_hunter_prints = 0;
BloodSpot g_blood_spots[kBloodSpots] = {};
int g_next_blood_spot = 0;
int g_hunter_trail = -2;              // unknown until it's looked up
float g_last_print[3] = {};
double g_last_print_time = 0.0;
alignas(16) char g_feet[kFeetSize] = {};
std::unordered_map<char*, Escape> g_escapes;
Bullet g_bullets[kBullets] = {};
int g_next_bullet = 0;
unsigned g_scared = 0;
unsigned g_ticks_run = 0;
unsigned g_ticks_saved = 0;
double g_next_report = 0.0;
float g_last_look_place[3] = {};
double g_next_look = 0.0;
double g_next_carcass_look = 0.0;
std::unordered_map<char*, Wild> g_wild;
std::vector<float> g_water;

Alarm g_alarms[kAlarms] = {};
int g_next_alarm = 0;

double g_last_shot = -100.0;
float g_shot_place[3] = {};
bool g_shot_heard = false;
bool g_alarm_nearby = false;

char* g_birds = nullptr;
double g_startled_until = 0.0;
double g_next_startle = 0.0;
float g_calm_period = 0.0f;
unsigned g_calm_most = 0;
std::vector<Carcass> g_carcasses;
bool g_vultures_called = false;
float g_bird_chances[kBirdKinds] = {};

template <typename T>
T& field(void* object, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<char*>(object) + offset);
}

template <typename Fn>
Fn engine(uintptr_t slot) {
    return *reinterpret_cast<Fn*>(g_game + slot);
}

const char* text_of(void* object, const char* key) {
    const char* value = engine<GetValueFn>(kGetValue)(object, nullptr, key, static_cast<unsigned>(-1));
    return value ? value : "";
}

float roll() { return static_cast<float>(rand()) / (static_cast<float>(RAND_MAX) + 1.0f); }

const Kind* kind_named(const char* name, size_t length) {
    for (const Kind& kind : kKinds) {
        if (strlen(kind.name) == length && !strncmp(kind.name, name, length)) return &kind;
    }
    return nullptr;
}

// Names are "wtail_buck00", or that with "sp12_" in front for an animal brought in later.
const Kind* kind_of_animal(const char* name) {
    if (name[0] == 's' && name[1] == 'p' && name[2] >= '0' && name[2] <= '9') {
        const char* after = strchr(name, '_');
        if (after) name = after + 1;
    }
    const char* end = strchr(name, '_');
    return end ? kind_named(name, end - name) : nullptr;
}

float squared_distance(const float* a, const float* b) {
    const float x = a[0] - b[0];
    const float z = a[2] - b[2];
    return x * x + z * z;
}

// What maximal_herd_size has to be for the generator's biggest group to be `largest`. A roll is
// always under 1, so it can only reach 1 less than the setting.
int herd_setting(int largest) { return largest + 1; }

int biggest_group(int setting, float value) { return static_cast<int>((value * kGroupShare + kLeastGroupShare) * setting); }

// `size` picks an entry past the three densities, which is how one herd of an exact size is asked for.
std::string counts_text(const Kind& kind) {
    char text[32];
    snprintf(text, sizeof(text), "%d,%d,%d", kind.low, kind.medium, kind.high);
    std::string counts = text;
    for (int size = 1; size <= kMostInAnyHerd; ++size) {
        snprintf(text, sizeof(text), ",%d", size);
        counts += text;
    }
    return counts;
}

void set_key(std::string* text, size_t from, size_t to, const char* key, const std::string& value) {
    const std::string wanted = std::string(key) + ":\"";
    const size_t at = text->find(wanted, from);
    if (at != std::string::npos && at < to) {
        const size_t start = at + wanted.size();
        text->replace(start, text->find('"', start) - start, value);
        return;
    }
    const size_t line = text->find('\n', from);
    if (line != std::string::npos) text->insert(line + 1, "\t\t" + wanted + value + "\"\r\n");
}

void replace_every(std::string* text, const char* from, const char* to) {
    const size_t length = strlen(from);
    for (size_t at = text->find(from); at != std::string::npos; at = text->find(from, at + strlen(to))) {
        text->replace(at, length, to);
    }
}

void set_type_sound(std::string* text, const char* species, const char* type, const char* sound) {
    const std::string opening = std::string("def ") + species + ":/species_kind";
    const size_t from = text->find(opening);
    if (from == std::string::npos) return;
    size_t to = text->find(":/species_kind", from + opening.size());
    if (to == std::string::npos) to = text->size();
    const size_t at = text->find(std::string("def ") + type + ":/species_type", from);
    if (at == std::string::npos || at >= to) return;
    set_key(text, at, to, "s_specific", sound);
}

bool in_window(double time, float phase, double every, double length) {
    return fmod(time + static_cast<double>(phase) * every, every) < length;
}

DayPart day_part(int hour) {
    if (hour >= kNightFrom || hour < kNightTo) return DayPart::Night;
    if ((hour >= kDawnFrom && hour < kDawnTo) || (hour >= kDuskFrom && hour < kDuskTo)) return DayPart::Peak;
    if (hour >= kQuietFrom && hour < kQuietTo) return DayPart::Quiet;
    return DayPart::Day;
}

void drink_times(DayPart part, bool hunts, double* every, double* length) {
    const bool quiet = part == DayPart::Quiet || (part == DayPart::Night && !hunts);
    *every = quiet ? kQuietDrinkEvery : kDrinkEvery;
    *length = part == DayPart::Peak ? kPeakDrinkFor : quiet ? kQuietDrinkFor : kDrinkFor;
}

void time_of_day(DayPart part, bool hunts, float* hunger, float* rest) {
    *hunger = 1.0f;
    *rest = 1.0f;
    if (part == DayPart::Peak) {
        *hunger = kPeakHunger;
        *rest = kPeakRest;
    } else if (part == DayPart::Quiet) {
        *hunger = kQuietHunger;
        *rest = kQuietRest;
    } else if (part == DayPart::Night) {
        *hunger = hunts ? kProwlHunger : kNightHunger;
        *rest = hunts ? kProwlRest : kNightRest;
    }
}

bool music_plays(int kind, bool free_hunt) { return kind != kAnimalAttacksMusic && !(free_hunt && kind == kTargetNearMusic); }

bool chase_music_waits(int kind, float distance) { return kind == kHunterAttackedMusic && distance > kChaseMusicReach; }

// A hunter goes by whether it's been provoked, as being a little afraid is what makes it attack.
bool frightened_off_water(bool hunts, float hunter_fear, float enemy_fear, bool provoked) {
    return hunts ? provoked : fmaxf(hunter_fear, enemy_fear) >= kAlarmFear;
}

float tick_gap(float distance, bool looked_at) {
    if (distance < kEveryFrame) return 0.0f;
    const float gap = distance < kNearLater ? kNearGap : distance < kFarLater ? kFarGap : kFurthestGap;
    return looked_at && gap > kNearGap ? kNearGap : gap;
}

// The game's table adds up as it goes, and a roll takes the first entry it's under.
void favour_bird(float* chances, const float* calm, int count, int favoured, float share) {
    float running = 0.0f;
    float last = 0.0f;
    const float total = calm[count - 1];
    for (int i = 0; i < count; ++i) {
        const float weight = calm[i] - last;
        last = calm[i];
        running += weight * (1.0f - share) + (i == favoured ? total * share : 0.0f);
        chances[i] = running;
    }
}

Wild& wild_for(char* animal);

bool in_scope_view(const float* place) {
    if (!g_zoomed) return false;
    const float x = place[0] - g_hunter[0];
    const float z = place[2] - g_hunter[2];
    const float length = sqrtf(x * x + z * z);
    return length > 0.0f && (x * g_forward[0] + z * g_forward[2]) / length >= kInScope;
}

// Spends stamina running and gets it back resting, and says how much faster this tick should go.
void run_pace(char* animal, Wild& wild, float seconds, float* legs, float* stride) {
    const int processor = field<int>(animal + kAnimalAnimations, kControllerProcessor);
    wild.running = processor == kAttackProcessor || processor == kRunAwayProcessor;
    const bool hurt = field<bool>(animal, kAnimalHurt);
    if (!wild.gait || !wild.running) {
        wild.stamina = fminf(1.0f, wild.stamina + seconds / kRecoverSeconds);
        return;
    }
    const Gait& gait = *wild.gait;
    wild.stamina = fmaxf(0.0f, wild.stamina - seconds * (hurt ? kWoundedDrain : 1.0f) / gait.stamina);
    float speed = gait.top * tired_share(wild.stamina);
    if (hurt) speed *= kWoundedPace;
    if (wild.young || g_now - wild.young_seen < kYoungSeenSeconds) speed *= kYoungPace;
    pace_shares(gait.run, fmaxf(speed, gait.run), legs, stride);
}

void __fastcall hook_animal_tick(char* animal, void* unused, float seconds) {
    if (!g_active) {
        g_real_tick(animal, unused, seconds);
        return;
    }
    Wild& wild = wild_for(animal);
    wild.waiting += seconds;
    const float* place = reinterpret_cast<float*>(animal + kAnimalPosition);
    if (wild.waiting < tick_gap(sqrtf(squared_distance(place, g_hunter)), in_scope_view(place))) {
        ++g_ticks_saved;
        return;
    }
    const float due = wild.waiting;
    wild.waiting = 0.0f;
    ++g_ticks_run;
    float legs = 1.0f, stride = 1.0f;
    run_pace(animal, wild, due, &legs, &stride);
    float& rate = field<float>(animal, kAnimalAnimationRate);
    float& print_gap = field<float>(animal, kAnimalPrintGap);
    const float kept_rate = rate, kept_print_gap = print_gap;
    rate *= legs;
    if (wild.running) print_gap *= kRunningPrintGap;
    g_stride = stride;
    g_real_tick(animal, unused, due);
    rate = kept_rate;
    print_gap = kept_print_gap;
    g_stride = 1.0f;
}

void __fastcall hook_music(void* hunt, void* unused, int kind, float volume) {
    const bool plays = music_plays(kind, g_on_hunt) || !wildlife_available();
    if (!plays) {
        logf("wildlife: music %d kept quiet", kind);
        ++g_music_kept_quiet;
        return;
    }
    if (g_thinking && wildlife_available()) {
        const float distance = sqrtf(squared_distance(reinterpret_cast<float*>(g_thinking + kAnimalPosition), g_hunter));
        const char* name = field<const char*>(g_thinking, kAnimalNameText);
        const bool waits = chase_music_waits(kind, distance);
        logf("wildlife: music %d %s, for %s %.0f m away", kind, waits ? "held back" : "played", name ? name : "an animal", distance);
        if (waits) {
            wild_for(g_thinking).chase_music_held = true;
            g_music_hunt = hunt;
            return;
        }
    } else {
        logf("wildlife: music %d played", kind);
    }
    g_real_music(hunt, unused, kind, volume);
}

float __fastcall hook_group_roll(void* random, void* unused) {
    if (g_whole_herd) return 1.0f;
    const float value = engine<RandomFn>(kRandom)(random, unused);
    return value * value;  // a lot of small groups, and now and then a very big one
}

void __fastcall hook_fire(void* hunter, void* unused, void* place, void* turn, void* sway, void* bullet, void* drop) {
    reinterpret_cast<FireFn>(g_game + kFire)(hunter, unused, place, turn, sway, bullet, drop);
    g_last_shot = now_seconds();
    memcpy(g_shot_place, g_hunter, sizeof(g_shot_place));
    g_shot_heard = true;
}

struct Member {
    char* definition;
    float place[3];
};

struct Types {
    char* female = nullptr;
    char* young = nullptr;
};

Types types_of(char* definitions, const char* species_name) {
    Types types;
    char* all = engine<FindInPoolFn>(kFindInPool)(definitions, nullptr, "species", static_cast<unsigned>(-1), false);
    if (!all) return types;
    char* species = engine<FindInPoolFn>(kFindInPool)(field<void*>(all, kObjectChildren), nullptr, species_name, static_cast<unsigned>(-1), false);
    if (!species) return types;
    void* children = field<void*>(species, kObjectChildren);
    for (char* entry = field<char*>(children, kPoolEntries); entry; entry = field<char*>(entry, kEntryNext)) {
        char* type = field<char*>(entry, kEntryObject);
        const int gender = atoi(text_of(type, "gender"));
        if (gender == 2) types.female = type;
        else if (gender == 3) types.young = type;
    }
    return types;
}

void set_weight(char* definition, char* type) {
    const char* range = text_of(type, "weight_range");
    const float least = static_cast<float>(atof(range));
    const char* colon = strchr(range, ':');
    const float most = colon ? static_cast<float>(atof(colon + 1)) : least;
    char text[32];
    snprintf(text, sizeof(text), "%.5f", least + (most - least) * roll());
    engine<SetValueFn>(kSetValue)(definition, nullptr, "weight", text, static_cast<unsigned>(-1));
}

void become(char* definition, char* type) {
    engine<SetValueFn>(kSetValue)(definition, nullptr, "type", type, static_cast<unsigned>(-1));
    set_weight(definition, type);
}

// Cows of some species have horns, picked from a longer list than their young have.
void make_young(char* definition, char* type) {
    become(definition, type);
    void* children = field<void*>(definition, kObjectChildren);
    for (const char* side : {"left_antler", "right_antler"}) {
        char* horn = engine<FindInPoolFn>(kFindInPool)(children, nullptr, side, static_cast<unsigned>(-1), false);
        if (horn) engine<SetValueFn>(kSetValue)(horn, nullptr, "main_beam", kYoungHorns, static_cast<unsigned>(-1));
    }
}

// A group's first female is its mother. Young follow her, as the game has them follow a mate.
int add_young(std::vector<Member>& group, const Kind& kind, const Types& types) {
    char* mother = nullptr;
    int young = 0;
    for (Member& member : group) {
        const bool female = types.female && !strcmp(text_of(member.definition, "type"), types.female);
        if (kind.family && !mother) {
            if (!female && types.female) become(member.definition, types.female);
            mother = member.definition;
            // She may have been made to follow another of the group, which would make her look like one of her own young.
            engine<SetValueFn>(kSetValue)(mother, nullptr, "mate", "", static_cast<unsigned>(-1));
            continue;
        }
        if (female && !mother) {
            mother = member.definition;
            continue;
        }
        if (!mother || young >= (kind.family ? kMostYoung : static_cast<int>(group.size())) || roll() >= kind.young) continue;
        if (kind.antlers && !female) continue;
        make_young(member.definition, types.young);
        engine<SetValueFn>(kSetValue)(member.definition, nullptr, "mate", mother, static_cast<unsigned>(-1));
        ++young;
    }
    return young;
}

// The game keeps every group on a 10 m circle, which is a crush for fifteen elk.
void spread_group(std::vector<Member>& group) {
    if (static_cast<int>(group.size()) <= kAlwaysClose) return;
    float middle[2] = {};
    for (const Member& member : group) {
        middle[0] += member.place[0];
        middle[1] += member.place[2];
    }
    middle[0] /= group.size();
    middle[1] /= group.size();
    const float scale = (kCloseGroup + group.size() * kSpreadPerAnimal) / kGameGroupRadius;
    for (Member& member : group) {
        const float moved[3] = {middle[0] + (member.place[0] - middle[0]) * scale + (roll() - 0.5f) * kCloseGroup, member.place[1],
                                middle[1] + (member.place[2] - middle[1]) * scale + (roll() - 0.5f) * kCloseGroup};
        engine<SetPointFn>(kSetPoint)(member.definition, nullptr, "position", moved, static_cast<unsigned>(-1));
    }
}

bool climbs(const Kind* kind) {
    if (!kind) return false;
    for (const char* name : kClimbers) {
        if (!strcmp(kind->name, name)) return true;
    }
    return false;
}

bool high_ground_safely(const float* from, float reach, const float* hunter, float nearest, float* best);
float ground_at(const float* at, float fallback);

void settle(void* animals, char* definitions) {
    std::vector<std::vector<Member>> groups;
    for (char* entry = field<char*>(animals, kPoolEntries); entry; entry = field<char*>(entry, kEntryNext)) {
        Member member;
        member.definition = field<char*>(entry, kEntryObject);
        engine<GetPointFn>(kGetPoint)(member.definition, nullptr, member.place, "position", static_cast<unsigned>(-1));
        const bool same = !groups.empty() && !strcmp(text_of(groups.back()[0].definition, "species"), text_of(member.definition, "species")) &&
                          squared_distance(groups.back()[0].place, member.place) < kSameGroup * kSameGroup;
        if (!same) groups.emplace_back();
        groups.back().push_back(member);
    }
    int young = 0;
    for (std::vector<Member>& group : groups) {
        const char* species = text_of(group[0].definition, "species");
        const Kind* kind = kind_named(species, strlen(species));
        if (!kind) continue;
        spread_group(group);
        if (climbs(kind)) {
            float middle[3] = {};
            for (const Member& member : group) {
                for (int i = 0; i < 3; ++i) middle[i] += member.place[i] / group.size();
            }
            float high[3];
            if (high_ground_safely(middle, kClimbSettleReach, nullptr, 0.0f, high)) {
                for (Member& member : group) {
                    float moved[3] = {high[0] + member.place[0] - middle[0], 0.0f, high[2] + member.place[2] - middle[2]};
                    moved[1] = ground_at(moved, high[1]);
                    engine<SetPointFn>(kSetPoint)(member.definition, nullptr, "position", moved, static_cast<unsigned>(-1));
                    memcpy(member.place, moved, sizeof(moved));
                }
                ++g_climbed;
                g_climbed_most = fmaxf(g_climbed_most, high[1] - middle[1]);
            }
        }
        if (group.size() < 2) continue;
        const Types types = types_of(definitions, species);
        if (types.young) young += add_young(group, *kind, types);
    }
    if (groups.size() > 1) logf("wildlife: %d groups with %d young", static_cast<int>(groups.size()), young);
    if (g_climbed > 0) logf("wildlife: %u goat herds moved up onto high ground, the most %.0f m up", g_climbed, g_climbed_most);
    g_climbed = 0;
    g_climbed_most = 0.0f;
}

Wild& wild_for(char* animal) {
    const auto found = g_wild.find(animal);
    if (found != g_wild.end()) return found->second;
    Wild wild;
    const char* name = field<const char*>(animal, kAnimalNameText);
    if (name) wild.kind = kind_of_animal(name);
    wild.voice = voice_of(wild.kind);
    wild.male = male_name(name);
    wild.gait = gait_of(wild.kind);
    wild.young = young_name(name);
    if (climbs(wild.kind)) {
        wild.has_home = true;
        memcpy(wild.home, animal + kAnimalPosition, sizeof(wild.home));
    }
    // A herd keeps the same hours, as they all follow the one animal.
    char* leader = field<char*>(animal, kAnimalMate);
    const uintptr_t seed = reinterpret_cast<uintptr_t>(leader ? leader : animal);
    wild.phase = static_cast<float>((seed >> 4) * 2654435761u % 1000u) / 1000.0f;
    return g_wild.emplace(animal, wild).first->second;
}

void raise_alarm(const float* place, float strength, double now) {
    Alarm& alarm = g_alarms[g_next_alarm];
    g_next_alarm = (g_next_alarm + 1) % kAlarms;
    memcpy(alarm.place, place, sizeof(alarm.place));
    alarm.strength = strength;
    alarm.time = now;
    if (squared_distance(place, g_hunter) < kBirdsNoticeAlarm * kBirdsNoticeAlarm) g_alarm_nearby = true;
}

// Fear of the hunter spreads through a herd, weaker each time, so it runs out before it reaches the
// whole map. Fear of a predator isn't passed on, or it'd turn into fear of the hunter, and that's
// enough for a bull a long way off to come for you.
bool catch_panic(char* brain, Wild& wild, const float* place) {
    float& hunter_fear = field<float>(brain, kBrainHunterFear);
    if (hunter_fear >= kAlarmFear && g_now >= wild.next_alarm) {
        wild.next_alarm = g_now + kAlarmGap;
        raise_alarm(place, hunter_fear * kAlarmFade, g_now);
    }
    bool caught = false;
    for (const Alarm& alarm : g_alarms) {
        if (g_now - alarm.time > kAlarmSeconds || alarm.strength <= hunter_fear) continue;
        if (squared_distance(place, alarm.place) >= kAlarmReach * kAlarmReach) continue;
        hunter_fear = alarm.strength;
        caught = true;
    }
    return caught;
}

// An animal's own number from 0 to 1, where phase is the same for the whole herd.
float own_roll(const char* animal) {
    return static_cast<float>((reinterpret_cast<uintptr_t>(animal) >> 4) * 2246822519u % 1000u) / 1000.0f;
}

// Its own sound, where it is, if it's near enough to be heard.
bool call_out(char* animal, bool with_animation) {
    __try {
        if (field<unsigned>(animal, kAnimalSoundCount) <= kCallSound) return false;
        char* sound = field<char**>(animal, kAnimalSounds)[kCallSound];
        if (!sound || !(field<unsigned char>(sound, kUnitFlags) & kUnitAlive)) return false;
        if (with_animation) {
            reinterpret_cast<PlayAnimationFn>(g_game + kPlayAnimation)(animal + kAnimalAnimations, nullptr, kCallAnimation, 1, 0);
        }
        reinterpret_cast<PlayCallFn>(g_game + kPlayCall)(animal, nullptr);
        ++g_calls;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Some stains of blood around an animal.
bool drop_blood(char* animal, int stain, int count) {
    __try {
        for (int i = 0; i < count; ++i) {
            reinterpret_cast<DropBloodFn>(g_game + kDropBlood)(animal, nullptr, stain);
            ++g_drops;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int drops_for(float bleeding) {
    const int count = 1 + static_cast<int>(bleeding > 0.0f ? bleeding * kDropsPerBleed : 0.0f);
    return count < kMostDrops ? count : kMostDrops;
}

void __fastcall hook_drip(char* animal, void*, int stain) {
    drop_blood(animal, stain, g_active ? drops_for(field<float>(animal, kAnimalBleeding)) : 1);
}

void __fastcall hook_hit_blood(char* animal, void*, int stain) { drop_blood(animal, stain, g_active ? kHitDrops : 1); }

// Twice in the same place, so the ground under it's darker.
bool __fastcall hook_stamp(void* stickers, void* unused, int trail, int side, float* at, float* up, float* way, float scale, int limit) {
    const auto stamp = reinterpret_cast<StampFn>(g_game + kStamp);
    const bool placed = stamp(stickers, unused, trail, side, at, up, way, scale, limit);
    if (placed && g_active) stamp(stickers, unused, trail, side, at, up, way, scale, limit);
    return placed;
}

void mark_blood(const float* place) {
    BloodSpot& spot = g_blood_spots[g_next_blood_spot];
    g_next_blood_spot = (g_next_blood_spot + 1) % kBloodSpots;
    memcpy(spot.place, place, sizeof(spot.place));
    spot.time = g_now;
}

// The newest blood a hunter could smell from here, or none if it's nearly on top of what's bleeding.
const float* blood_scent(const float* place) {
    const BloodSpot* newest = nullptr;
    for (const BloodSpot& spot : g_blood_spots) {
        if (spot.time <= 0.0 || g_now - spot.time > kBloodScentSeconds) continue;
        if (squared_distance(place, spot.place) > kBloodScent * kBloodScent) continue;
        if (!newest || spot.time > newest->time) newest = &spot;
    }
    if (!newest || squared_distance(place, newest->place) < kBloodFound * kBloodFound) return nullptr;
    return newest->place;
}

// The hunter's own prints, put down by the same function as an animal's, handed only what it reads.
bool leave_print(const float* at, const float* way) {
    __try {
        if (g_hunter_trail == -2) {
            char* stickers = reinterpret_cast<StickersFn>(g_game + kStickers)(reinterpret_cast<CurrentHuntFn>(g_game + kCurrentHunt)(), nullptr);
            const char* name = kHunterTrail;
            g_hunter_trail = stickers ? reinterpret_cast<StickerNamedFn>(g_game + kStickerNamed)(stickers, nullptr, &name) : -1;
        }
        if (g_hunter_trail < 0) return false;
        field<float>(g_feet, kFeetMoving) = 1.0f;
        field<int>(g_feet, kFeetGround) = kSoftGround;
        memcpy(g_feet + kFeetFrom, at, 3 * sizeof(float));
        memcpy(g_feet + kFeetTo, at, 3 * sizeof(float));
        memcpy(g_feet + kAnimalPosition, at, 3 * sizeof(float));
        memcpy(g_feet + kFeetWay, way, 3 * sizeof(float));
        field<float>(g_feet, kAnimalPrintGap) = kHunterSideStep;
        field<float>(g_feet, kFeetScale) = 1.0f;
        field<int>(g_feet, kFeetTrail) = g_hunter_trail;
        reinterpret_cast<PlacePrintFn>(g_game + kPlacePrint)(g_feet, nullptr);
        ++g_hunter_prints;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_hunter_trail = -1;
        return false;
    }
}

// A print every stride on foot.
void leave_prints() {
    const double seconds = g_now - g_last_print_time;
    const float x = g_hunter[0] - g_last_print[0];
    const float z = g_hunter[2] - g_last_print[2];
    const float moved = sqrtf(x * x + z * z);
    if (!freelook_on_foot() || g_last_print_time <= 0.0 || seconds <= 0.0 || moved / static_cast<float>(seconds) > kBigMoveSpeed) {
        memcpy(g_last_print, g_hunter, sizeof(g_last_print));
        g_last_print_time = g_now;
        return;
    }
    if (moved < kHunterStride) return;
    const float way[3] = {x, 0.0f, z};
    leave_print(g_hunter, way);
    memcpy(g_last_print, g_hunter, sizeof(g_last_print));
    g_last_print_time = g_now;
}

// Multiplies the number in each key:"n", or keyNN:"n", between from and to.
void scale_key(std::string* text, size_t from, size_t to, const char* key, float factor) {
    const size_t length = strlen(key);
    for (size_t at = text->find(key, from); at != std::string::npos && at < to; at = text->find(key, at + 1)) {
        size_t value = at + length;
        while (value < text->size() && (*text)[value] >= '0' && (*text)[value] <= '9') ++value;
        if (text->compare(value, 2, ":\"") != 0) continue;
        value += 2;
        const size_t end = text->find('"', value);
        if (end == std::string::npos || end > to) return;
        char number[32];
        snprintf(number, sizeof(number), "%.3f", atof(text->substr(value, end - value).c_str()) * factor);
        text->replace(value, end - value, number);
        to = to + strlen(number) - (end - value);
    }
}

char* herd_leader(char* animal) {
    char* leader = field<char*>(animal, kAnimalMate);
    return leader ? leader : animal;
}

// 0, then one step to each side, then two, and so on.
float flee_angle(int index) {
    const int steps = (index + 1) / 2;
    return steps * kFleeAngleStep * (index % 2 ? 1.0f : -1.0f);
}

float shot_wakes(float distance) {
    const float wake = kShotWakes * (1.0f - distance / kShotWakeReach);
    return wake < 0.0f ? 0.0f : wake > 1.0f ? 1.0f : wake;
}

float segment_distance(const float* place, const float* from, const float* to) {
    float along[3], off[3];
    for (int i = 0; i < 3; ++i) {
        along[i] = to[i] - from[i];
        off[i] = place[i] - from[i];
    }
    const float length = along[0] * along[0] + along[1] * along[1] + along[2] * along[2];
    float share = length > 0.0f ? (off[0] * along[0] + off[1] * along[1] + off[2] * along[2]) / length : 0.0f;
    share = share < 0.0f ? 0.0f : share > 1.0f ? 1.0f : share;
    float nearest[3];
    for (int i = 0; i < 3; ++i) nearest[i] = from[i] + along[i] * share;
    return sqrtf(squared_distance(place, nearest));
}

// Whether a bullet's gone past since it last looked.
bool near_miss(Wild& wild, const float* place) {
    bool missed = false;
    double newest = wild.last_bullet_seen;
    for (const Bullet& bullet : g_bullets) {
        if (bullet.time <= wild.last_bullet_seen || g_now - bullet.time > kBulletSeconds) continue;
        newest = bullet.time > newest ? bullet.time : newest;
        if (segment_distance(place, bullet.from, bullet.to) < kNearMiss) missed = true;
    }
    wild.last_bullet_seen = newest;
    return missed;
}

// How good a place is for a climber: high, and steep, from the ground there and 10 m round it.
bool climbing_score(const float* at, float look_from, float* ground, float* score) {
    float depth = 0.0f;
    const float high[3] = {at[0], look_from, at[2]};
    if (!free_hunt_water_depth(high, &depth, ground) || depth > 0.0f) return false;
    float steepest = 0.0f;
    const float offsets[4][2] = {{kSlopeStep, 0.0f}, {-kSlopeStep, 0.0f}, {0.0f, kSlopeStep}, {0.0f, -kSlopeStep}};
    for (const auto& offset : offsets) {
        const float near_by[3] = {at[0] + offset[0], look_from, at[2] + offset[1]};
        float other = 0.0f;
        if (!free_hunt_water_depth(near_by, &depth, &other)) continue;
        steepest = fmaxf(steepest, fabsf(other - *ground) / kSlopeStep);
    }
    *score = *ground + kSteepWorth * fminf(steepest, kSteepest);
    return true;
}

// The best place for a climber within reach of where it'd be, on the ground there, keeping at least
// nearest from you if you're known. False if nothing's better than where it is.
bool high_ground(const float* from, float reach, const float* hunter, float nearest, float* best) {
    const float look_from = from[1] + kClimbLookFrom;
    float ground = 0.0f, best_score = 0.0f;
    if (!climbing_score(from, look_from, &ground, &best_score)) best_score = -1e9f;
    bool found = false;
    for (float ring = kClimbStep; ring <= reach; ring += kClimbStep) {
        for (int spoke = 0; spoke < kClimbSpokes; ++spoke) {
            const float angle = spoke * kFullCircle / kClimbSpokes;
            float spot[3] = {from[0] + sinf(angle) * ring, 0.0f, from[2] + cosf(angle) * ring};
            if (hunter) {
                const float x = spot[0] - hunter[0], z = spot[2] - hunter[2];
                if (x * x + z * z < nearest * nearest) continue;
            }
            float score = 0.0f;
            if (!climbing_score(spot, look_from, &spot[1], &score) || score <= best_score) continue;
            best_score = score;
            memcpy(best, spot, sizeof(spot));
            found = true;
        }
    }
    return found;
}

bool high_ground_safely(const float* from, float reach, const float* hunter, float nearest, float* best) {
    __try {
        return high_ground(from, reach, hunter, nearest, best);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The ground at a place across, or the given height if it can't be found.
float ground_at(const float* at, float fallback) {
    const float high[3] = {at[0], fallback + kGroundFromAbove, at[2]};
    float depth = 0.0f, ground = 0.0f;
    __try {
        if (free_hunt_water_depth(high, &depth, &ground)) return ground;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return fallback;
}

float clamped_climb(float climb) {
    const float share = climb / kHillScale;
    return share < -1.0f ? -1.0f : share > 1.0f ? 1.0f : share;
}

// Straight away is best, then what climbs, and nothing that runs into water. False if every line
// does, and then it's straight away anyway.
bool escape_line(const float* place, const float* threat, float* heading, float hill_pull) {
    float away[2] = {place[0] - threat[0], place[2] - threat[2]};
    const float length = sqrtf(away[0] * away[0] + away[1] * away[1]);
    if (length > 0.01f) {
        away[0] /= length;
        away[1] /= length;
    } else {
        away[0] = 1.0f;
        away[1] = 0.0f;
    }
    heading[0] = away[0];
    heading[1] = away[1];
    const float high = place[1] + kLookFromAbove;
    const float here[3] = {place[0], high, place[2]};
    float depth = 0.0f, here_ground = 0.0f;
    if (!free_hunt_water_depth(here, &depth, &here_ground)) return true;
    float best = -1e9f;
    bool found = false;
    for (int i = 0; i < kFleeAngles; ++i) {
        const float angle = flee_angle(i);
        const float c = cosf(angle), s = sinf(angle);
        const float way[2] = {away[0] * c - away[1] * s, away[0] * s + away[1] * c};
        bool dry = true;
        float ground = here_ground;
        for (float along = kFleeLook * 0.5f; along <= kFleeLook; along += kFleeLook * 0.5f) {
            const float spot[3] = {place[0] + way[0] * along, high, place[2] + way[1] * along};
            dry = dry && free_hunt_water_depth(spot, &depth, &ground) && depth <= kDeepestDrink;
        }
        if (!dry) continue;
        const float score = c + hill_pull * clamped_climb(ground - here_ground);
        if (score <= best) continue;
        best = score;
        heading[0] = way[0];
        heading[1] = way[1];
        found = true;
    }
    return found;
}

bool escape_line_safely(const float* place, const float* threat, float* heading, bool* cornered, float hill_pull = kHillPull) {
    __try {
        *cornered = !escape_line(place, threat, heading, hill_pull);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Dry, higher up and further from you, within 50 m.
bool best_bed(const float* place, float* bed) {
    const float high = place[1] + kLookFromAbove;
    const float here[3] = {place[0], high, place[2]};
    float depth = 0.0f, here_ground = 0.0f;
    if (!free_hunt_water_depth(here, &depth, &here_ground)) return false;
    const float from_you = sqrtf(squared_distance(place, g_hunter));
    float best = -1e9f;
    bool found = false;
    for (const float ring : kBedRings) {
        for (int spoke = 0; spoke < kBedSpokes; ++spoke) {
            const float angle = spoke * 6.2831853f / kBedSpokes;
            const float spot[3] = {place[0] + sinf(angle) * ring, high, place[2] + cosf(angle) * ring};
            float ground = 0.0f;
            if (!free_hunt_water_depth(spot, &depth, &ground) || depth > kDriestBed) continue;
            const float level[3] = {spot[0], ground, spot[2]};
            const float away = (sqrtf(squared_distance(level, g_hunter)) - from_you) / ring;
            const float score = clamped_climb(ground - here_ground) + kBedAway * away;
            if (score <= best) continue;
            best = score;
            memcpy(bed, level, sizeof(level));
            found = true;
        }
    }
    return found;
}

bool best_bed_safely(const float* place, float* bed) {
    __try {
        return best_bed(place, bed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The herd lies around wherever the one it follows chose.
void choose_bed(char* animal, char* leader, Wild& wild, const float* place) {
    wild.has_bed = true;
    memcpy(wild.bed, place, sizeof(wild.bed));
    if (leader) {
        const Wild& led = wild_for(leader);
        if (led.has_bed) {
            const float angle = own_roll(animal) * 6.2831853f;
            wild.bed[0] = led.bed[0] + sinf(angle) * kBedSpread;
            wild.bed[1] = ground_at(wild.bed, led.bed[1]);
            wild.bed[2] = led.bed[2] + cosf(angle) * kBedSpread;
            return;
        }
    }
    best_bed_safely(place, wild.bed);
}

float hill_pull_of(char* animal) { return climbs(wild_for(animal).kind) ? kClimberHillPull : kHillPull; }

// Whether the herd's line would take this one towards what it's running from: it's on the far side.
bool line_runs_towards(const float* place, const float* threat, const float* line) {
    const float away[2] = {place[0] - threat[0], place[2] - threat[2]};
    const float length = sqrtf(away[0] * away[0] + away[1] * away[1]);
    return length > 0.01f && (line[0] * away[0] + line[1] * away[1]) / length < kLineToward;
}

float __fastcall hook_flee_target(char* animal, void* unused, const float* threat, float* target) {
    if (!g_active) return reinterpret_cast<FleeTargetFn>(g_game + kFleeTarget)(animal, unused, threat, target);
    const float* place = reinterpret_cast<float*>(animal + kAnimalPosition);
    Escape& escape = g_escapes[herd_leader(animal)];
    if (g_now >= escape.until || squared_distance(escape.threat, threat) > kThreatMoved * kThreatMoved) {
        bool cornered = false;
        if (!escape_line_safely(place, threat, escape.heading, &cornered, hill_pull_of(animal))) {
            return reinterpret_cast<FleeTargetFn>(g_game + kFleeTarget)(animal, unused, threat, target);
        }
        escape.cornered = cornered;
        memcpy(escape.threat, threat, sizeof(escape.threat));
        ++g_lines_picked;
        if (cornered) ++g_cornered;
    }
    escape.until = g_now + kEscapeKeeps;
    Wild& wild = wild_for(animal);
    ++wild.places_given;
    if (escape.cornered) wild.cornered_until = g_now + kCorneredSeconds;
    float line[2] = {escape.heading[0], escape.heading[1]};
    if (line_runs_towards(place, threat, line)) {
        bool cornered = false;
        if (escape_line_safely(place, threat, line, &cornered, hill_pull_of(animal))) ++g_own_lines;
    }
    float way[2] = {line[0], line[1]};
    float step = kFleeStep;
    if (g_now < wild.jink_until) {
        const float c = cosf(kJinkAngle * wild.jink_side), s = sinf(kJinkAngle * wild.jink_side);
        way[0] = line[0] * c - line[1] * s;
        way[1] = line[0] * s + line[1] * c;
        step = kJinkStep;
    }
    target[0] = place[0] + way[0] * step;
    target[2] = place[2] + way[1] * step;
    target[1] = ground_at(target, place[1]);
    return squared_distance(place, threat);
}

bool threat_place(char* animal, float* place);

// Which goal, how it's moving, and for something running away, from what and to where.
void describe(char* animal, const Wild& wild, char* note, size_t size) {
    char* brain = animal + kAnimalBrain;
    const float* place = reinterpret_cast<const float*>(animal + kAnimalPosition);
    const float* target = reinterpret_cast<const float*>(animal + kAnimalGoalPlace);
    const int processor = field<int>(animal + kAnimalAnimations, kControllerProcessor);
    const int processors = static_cast<int>(sizeof(kProcessorNames) / sizeof(kProcessorNames[0]));
    float threat[3] = {};
    const bool known = threat_place(animal, threat);
    snprintf(note, size, "%s, goal %d stage %d, %s, mode %d, %.0f m from you, %s %.0f m off, %u new places to run to in %.0f s, heading for one %.0f m off",
             wild.kind ? wild.kind->name : "an animal", field<int>(brain, kBrainGoal), field<int>(brain, kBrainGoalStage),
             processor >= 0 && processor < processors ? kProcessorNames[processor] : "?", field<int>(animal, kAnimalMoveMode),
             sqrtf(squared_distance(place, g_hunter)),
             !known ? "running from something unknown" : squared_distance(threat, g_hunter) < 25.0f ? "running from you," : "running from something else,",
             known ? sqrtf(squared_distance(place, threat)) : 0.0f, wild.places_given - wild.places_at_turn,
             g_now - wild.turning_since, sqrtf(squared_distance(place, target)));
}

// How far a heading turned, the short way round.
float turned_by(float from, float to) {
    float turn = to - from;
    while (turn > 3.14159265f) turn -= kFullCircle;
    while (turn < -3.14159265f) turn += kFullCircle;
    return turn;
}

// Sends something going round in circles to a place it can reach, off to one side.
void unstick(char* animal, Wild& wild, const float* place) {
    float threat[3];
    if (!threat_place(animal, threat)) memcpy(threat, g_hunter, sizeof(threat));
    float line[2] = {place[0] - threat[0], place[2] - threat[2]};
    bool cornered = false;
    if (!escape_line_safely(place, threat, line, &cornered, hill_pull_of(animal))) {
        const float length = sqrtf(line[0] * line[0] + line[1] * line[1]);
        line[0] = length > 0.01f ? line[0] / length : 1.0f;
        line[1] = length > 0.01f ? line[1] / length : 0.0f;
    }
    wild.unstick_side = -wild.unstick_side;
    const float c = cosf(kUnstickAngle * wild.unstick_side), s = sinf(kUnstickAngle * wild.unstick_side);
    float* target = reinterpret_cast<float*>(animal + kAnimalGoalPlace);
    target[0] = place[0] + (line[0] * c - line[1] * s) * kUnstickStep;
    target[2] = place[2] + (line[0] * s + line[1] * c) * kUnstickStep;
    target[1] = ground_at(target, place[1]);
    reinterpret_cast<RunToFn>(g_game + kRunTo)(animal, nullptr, false);
    ++g_unstuck;
}

bool unstick_safely(char* animal, Wild& wild, const float* place) {
    __try {
        unstick(animal, wild, place);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Something you're watching through binoculars that starts running away, and why.
void note_spooked(char* animal, const Wild& wild, const float* place) {
    if (g_spooked_notes >= kMostSpookedNotes) return;
    ++g_spooked_notes;
    char* brain = animal + kAnimalBrain;
    float threat[3] = {};
    const bool known = threat_place(animal, threat);
    logf("wildlife: %s you're watching started running away %.0f m from you: goal %d, fear of you %.2f and of others %.2f, %s %.0f m off",
         wild.kind ? wild.kind->name : "an animal", sqrtf(squared_distance(place, g_hunter)), field<int>(brain, kBrainGoal),
         field<float>(brain, kBrainHunterFear), field<float>(brain, kBrainEnemyFear),
         !known ? "from something unknown" : squared_distance(threat, g_hunter) < 25.0f ? "from you," : "from something else,",
         known ? sqrtf(squared_distance(place, threat)) : 0.0f);
}

// Every half second or so, which way it went and how fast. Far animals only move on their own ticks,
// so a frame's worth of moving says nothing.
void track_running(char* animal, Wild& wild, const float* place) {
    const double since = g_now - wild.track_time;
    if (wild.track_time <= 0.0 || since > kTrackLost) {
        memcpy(wild.track_place, place, sizeof(wild.track_place));
        wild.track_time = g_now;
        wild.has_heading = false;
        return;
    }
    if (since < kTrackSeconds) return;
    const int goal = field<int>(animal + kAnimalBrain, kBrainGoal);
    const bool running_away = (goal == kFleeGoal || goal == kEnemyGoal) && field<int>(animal, kAnimalMoveMode) == kFleeMode;
    if (running_away && !wild.was_running_away && in_scope_view(place)) note_spooked(animal, wild, place);
    wild.was_running_away = running_away;
    const float way[2] = {(place[0] - wild.track_place[0]) / static_cast<float>(since),
                          (place[2] - wild.track_place[2]) / static_cast<float>(since)};
    memcpy(wild.track_place, place, sizeof(wild.track_place));
    wild.track_time = g_now;
    const float pace = sqrtf(way[0] * way[0] + way[1] * way[1]);
    if (pace > g_fastest && wild.kind) {
        g_fastest = pace;
        g_fastest_kind = wild.kind->name;
    }
    if (!wild.running || pace < kTrackSpeed) {
        wild.has_heading = false;
        return;
    }
    const float heading = atan2f(way[1], way[0]);
    if (!wild.has_heading || g_now - wild.turning_since > kCircleWithin) {
        wild.turned = 0.0f;
        wild.turning_since = g_now;
        wild.places_at_turn = wild.places_given;
    } else {
        wild.turned += turned_by(wild.track_heading, heading);
    }
    wild.track_heading = heading;
    wild.has_heading = true;
    if (running_away && fabsf(wild.turned) >= kStuckTurn && wild.places_given == wild.places_at_turn) {
        describe(animal, wild, g_circle_note, sizeof(g_circle_note));
        ++g_circled;
        unstick_safely(animal, wild, place);
        wild.turned = 0.0f;
        wild.turning_since = g_now;
        wild.places_at_turn = wild.places_given;
    }
    if (fabsf(wild.turned) >= kFullCircle) {
        ++g_circled;
        describe(animal, wild, g_circle_note, sizeof(g_circle_note));
        wild.turned = 0.0f;
        wild.turning_since = g_now;
        wild.places_at_turn = wild.places_given;
    }
    const float to_you[2] = {g_hunter[0] - place[0], g_hunter[2] - place[2]};
    const float apart = sqrtf(to_you[0] * to_you[0] + to_you[1] * to_you[1]);
    const bool attacking = field<int>(animal + kAnimalBrain, kBrainGoal) == kAttackHunterGoal || wild.bluffing;
    if (!attacking && apart < kAtYouReach && apart > 0.0f && g_now >= wild.at_you_until &&
        (way[0] * to_you[0] + way[1] * to_you[1]) / (pace * apart) > kAtYou) {
        ++g_at_you;
        describe(animal, wild, g_at_you_note, sizeof(g_at_you_note));
        wild.at_you_until = g_now + kAtYouGap;
    }
}

// What it does when something runs into you, from how it's been moving.
void check_bumps(char* manager) {
    const unsigned count = field<unsigned>(manager, kManagerCount);
    char** animals = field<char**>(manager, kManagerAnimals);
    for (unsigned i = 0; i < count; ++i) {
        char* animal = animals[i];
        if (!animal || !(field<unsigned char>(animal, kUnitFlags) & kUnitAlive) || field<bool>(animal, kAnimalDead)) continue;
        const float* place = reinterpret_cast<float*>(animal + kAnimalPosition);
        Wild& wild = wild_for(animal);
        const double seconds = g_now - wild.last_place_time;
        float speed[2] = {};
        if (wild.last_place_time > 0.0 && seconds > 0.0 && seconds < kLongestStep) {
            speed[0] = (place[0] - wild.last_place[0]) / static_cast<float>(seconds);
            speed[1] = (place[2] - wild.last_place[2]) / static_cast<float>(seconds);
        }
        if (seconds > 0.0) {
            memcpy(wild.last_place, place, sizeof(wild.last_place));
            wild.last_place_time = g_now;
        }
        track_running(animal, wild, place);
        const float x = g_hunter[0] - place[0];
        const float z = g_hunter[2] - place[2];
        const float apart = sqrtf(x * x + z * z);
        if (apart >= kBumpReach || apart <= 0.0f || fabsf(g_hunter[1] - place[1]) > kBumpHeight || g_now < wild.next_bump) continue;
        const float closing = (speed[0] * x + speed[1] * z) / apart;
        if (closing < kBumpSpeed) continue;
        wild.next_bump = g_now + kBumpGap;
        const float moving = sqrtf(speed[0] * speed[0] + speed[1] * speed[1]);
        const float push[2] = {speed[0] / moving, speed[1] / moving};
        float view_place[3], forward[3];
        float back = 1.0f, side = 0.0f;
        if (free_hunt_read_view(view_place, forward)) {
            back = -(push[0] * forward[0] + push[1] * forward[2]);
            side = push[0] * -forward[2] + push[1] * forward[0];
        }
        const float strength = fmaxf(kSoftestBump, fminf(1.0f, closing / kHardBump));
        freelook_shove(push, back, side, strength);
        ++g_barged;
        const char* name = field<const char*>(animal, kAnimalNameText);
        logf("wildlife: %s ran into you at %.1f m/s", name ? name : "an animal", closing);
    }
}

// The game's sight is already scaled by the light level, so close up this takes that back out.
float night_sight_share(float distance, float darkness, float light_level, bool lit) {
    const float reach = lit ? kLitNightSight : kNightSight;
    const float up_close = distance < reach ? 1.0f / fmaxf(light_level, kDimmestLight) : 0.0f;
    return 1.0f + (up_close - 1.0f) * darkness;
}

// A lantern shows all around. A beam only shows from where it's pointing.
bool light_noticed(float x, float z, bool glow, bool beam, const float* way) {
    const float distance = sqrtf(x * x + z * z);
    if (glow && distance < kGlowDraws) return true;
    return beam && distance > 0.0f && distance < kBeamDraws && (x * way[0] + z * way[2]) / distance >= kBeamDrawsWithin;
}

// True while the animal should be walking to the light. Seeing the hunter ends that, and it's
// real fear, so it spreads through the herd as any other would.
bool follow_light(char* brain, Wild& wild, const float* place) {
    if (g_now < wild.lure_rests_until) return false;
    float& hunter_fear = field<float>(brain, kBrainHunterFear);
    if (fmaxf(hunter_fear, field<float>(brain, kBrainEnemyFear)) >= kCalm) return false;
    if (g_now < wild.lured_until && field<float>(brain, kBrainSight) >= kSeesYou) {
        hunter_fear = kLightScare;
        wild.lured_until = 0.0;
        wild.lure_rests_until = g_now + kLureRests;
        ++g_scared;
        return false;
    }
    if (light_noticed(place[0] - g_hunter[0], place[2] - g_hunter[2], lights_glow_on(), g_beam_read, g_forward)) {
        if (g_now >= wild.lured_until) ++g_drawn;
        wild.lured_until = g_now + kLureLasts;
    }
    return g_now < wild.lured_until;
}

void startle_birds() {
    if (!g_birds || g_now < g_next_startle) return;
    g_next_startle = g_now + kStartleGap;
    if (g_startled_until == 0.0) g_calm_period = field<float>(g_birds, kBirdPeriod);
    g_startled_until = g_now + kStartledSeconds;
    field<float>(g_birds, kBirdPeriod) = kStartledPeriod;
    field<float>(g_birds, kBirdTimer) = kStartledPeriod;
    field<unsigned>(g_birds, kBirdMost) = g_calm_most + kStartledBirds;
}

// Most birds roost at night, so fewer are about, and that's what startling them adds to.
void calm_birds() {
    g_calm_most = day_part(g_hour) == DayPart::Night ? kNightBirds : kDayBirds;
    if (g_startled_until != 0.0 && g_now < g_startled_until) return;
    if (g_startled_until != 0.0) field<float>(g_birds, kBirdPeriod) = g_calm_period;
    g_startled_until = 0.0;
    field<unsigned>(g_birds, kBirdMost) = g_calm_most;
}

void read_water(char* manager) {
    g_water.clear();
    const unsigned count = field<unsigned>(manager, kManagerAreaCount);
    char** areas = field<char**>(manager, kManagerAreas);
    for (unsigned i = 0; i < count; ++i) {
        if (field<int>(areas[i], kAreaResource) != kWaterResource) continue;
        const float* place = reinterpret_cast<float*>(areas[i] + kAreaPosition);
        g_water.insert(g_water.end(), place, place + 3);
    }
    g_birds = field<char*>(manager, kManagerBirds);
    g_areas_read = true;
    logf("wildlife: %u food and water areas, %d of them water", count, static_cast<int>(g_water.size() / 3));
}

void call_vultures(bool wanted) {
    if (wanted == g_vultures_called || !g_birds) return;
    if (field<int>(g_birds, kBirdChanceCount) != kBirdKinds) return;
    float* chances = field<float*>(g_birds, kBirdChances);
    if (wanted) {
        memcpy(g_bird_chances, chances, sizeof(g_bird_chances));
        favour_bird(chances, g_bird_chances, kBirdKinds, kVulture, kVultureShare);
    } else {
        memcpy(chances, g_bird_chances, sizeof(g_bird_chances));
    }
    g_vultures_called = wanted;
}

void look_for_carcasses(char* manager) {
    const unsigned count = field<unsigned>(manager, kManagerCount);
    char** animals = field<char**>(manager, kManagerAnimals);
    for (unsigned i = 0; i < count; ++i) {
        char* animal = animals[i];
        if (!animal || !(field<unsigned char>(animal, kUnitFlags) & kUnitAlive) || !field<bool>(animal, kAnimalDead)) continue;
        bool known = false;
        for (const Carcass& carcass : g_carcasses) known = known || carcass.animal == animal;
        if (known) continue;
        Carcass carcass;
        carcass.animal = animal;
        memcpy(carcass.place, animal + kAnimalPosition, sizeof(carcass.place));
        carcass.seen = g_now;
        g_carcasses.push_back(carcass);
        raise_alarm(carcass.place, kDeathAlarm, g_now);
        drop_blood(animal, kPoolStain, kPoolDrops);
    }
    bool near_one = false;
    for (const Carcass& carcass : g_carcasses) {
        near_one = near_one || (g_now - carcass.seen < kCarcassSeconds && squared_distance(carcass.place, g_hunter) < kCarcassReach * kCarcassReach);
    }
    call_vultures(near_one);
}

// Water stopped a bullet dead, with a splash, so whatever was in it couldn't be hit. Taking water out
// of what the ray found lets it carry on to that, or the bottom.
unsigned __fastcall hook_bullet_ray(void* world, void* unused, int mask, const float* from, const float* way, float length, char* hits) {
    const unsigned found = reinterpret_cast<RayCastFn>(g_game + kRayCast)(world, unused, mask, from, way, length, hits);
    const float size = sqrtf(way[0] * way[0] + way[1] * way[1] + way[2] * way[2]);
    if (size > 0.0f) {
        Bullet& bullet = g_bullets[g_next_bullet];
        g_next_bullet = (g_next_bullet + 1) % kBullets;
        for (int i = 0; i < 3; ++i) {
            bullet.from[i] = from[i];
            bullet.to[i] = from[i] + way[i] / size * length;
        }
        bullet.time = now_seconds();
    }
    char* first = field<char*>(hits, kHitsFirst);
    unsigned& count = field<unsigned>(hits, kHitsCount);
    unsigned kept = 0;
    for (unsigned i = 0; i < count; ++i) {
        char* hit = first + i * kHitSize;
        char* geom = field<char*>(hit, kHitGeom);
        if (geom && field<int>(geom, kGeomKind) == kWaterGeom) continue;
        if (kept != i) memmove(first + kept * kHitSize, hit, kHitSize);
        ++kept;
    }
    if (kept == count) return found;
    count = kept;
    return kept;
}

// Walks back from the water's middle towards the animal, to the first place it can stand.
bool shore_towards(const float* water, const float* from, float* shore) {
    float depth = 0.0f;
    const float high = fmaxf(water[1], from[1]) + kLookDownFrom;
    const float middle[3] = {water[0], high, water[2]};
    if (!free_hunt_water_depth(middle, &depth) || depth <= kDeepestDrink) return false;
    const float x = from[0] - water[0];
    const float z = from[2] - water[2];
    const float length = sqrtf(x * x + z * z);
    for (float along = kShoreStep; along < length && along < kShoreSearch; along += kShoreStep) {
        const float place[3] = {water[0] + x / length * along, high, water[2] + z / length * along};
        if (!free_hunt_water_depth(place, &depth) || depth > kDeepestDrink) continue;
        shore[0] = place[0];
        shore[1] = water[1];
        shore[2] = place[2];
        return true;
    }
    return false;
}

bool shore_towards_safely(const float* water, const float* from, float* shore) {
    __try {
        return shore_towards(water, from, shore);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall hook_water_target(void* manager, void* unused, int area, char* target) {
    reinterpret_cast<AreaTargetFn>(g_game + kAreaTarget)(manager, unused, area, target);
    if (!g_active) return;
    float* place = reinterpret_cast<float*>(target + kTargetPlace);
    const float* from = reinterpret_cast<float*>(target - kAnimalTarget + kAnimalPosition);
    float shore[3];
    if (!shore_towards_safely(place, from, shore)) return;
    memcpy(place, shore, sizeof(shore));
    field<float>(target, kTargetNear) = kAtTheEdge;
    ++g_led_to_shore;
}

void __fastcall hook_root_motion(void* model, void* unused, float* move, float* turn) {
    reinterpret_cast<RootMotionFn>(g_game + kRootMotion)(model, unused, move, turn);
    if (g_stride == 1.0f) return;
    for (int i = 0; i < 3; ++i) move[i] *= g_stride;
}

// The only call through this is an animal's facing coming round towards its way.
void __fastcall hook_turn(float* facing, void* unused, const float* from, const float* to, float share) {
    if (g_active) {
        const auto found = g_wild.find(reinterpret_cast<char*>(facing) - kAnimalRotation);
        if (found != g_wild.end() && found->second.gait) {
            const float extra = found->second.gait->turn - 1.0f;
            share *= 1.0f + extra * (found->second.running ? 1.0f : kWalkTurnShare);
            share = share < 1.0f ? share : 1.0f;
        }
    }
    g_real_slerp(facing, unused, from, to, share);
}

// Where whatever it's running from is, through the game's own pointer to it.
bool threat_place(char* animal, float* place) {
    __try {
        void* threat = field<void*>(animal, kAnimalThreat);
        if (!threat) return false;
        const float* at = reinterpret_cast<ThreatPlaceFn>(field<void**>(threat, 0)[1])(threat, nullptr, place);
        if (at != place) memcpy(place, at, 3 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// In the dark, an animal goes by what it can hear.
void __fastcall hook_senses(char* brain, void* unused) {
    const auto real = reinterpret_cast<SensesFn>(g_game + kSenses);
    const float darkness = g_active ? daytime_darkness() : 0.0f;
    if (darkness <= 0.0f) {
        real(brain, unused);
        return;
    }
    char* senses = reinterpret_cast<ManagerFn>(g_game + kAnimalManager)() + kManagerSenses;
    float& sight = field<float>(senses, kSensesSight);
    float& smell = field<float>(senses, kSensesSmell);
    float& hearing = field<float>(senses, kSensesHearing);
    const float kept_sight = sight, kept_smell = smell, kept_hearing = hearing;
    char* animal = field<char*>(brain, kBrainAnimal);
    const float* place = reinterpret_cast<float*>(animal + kAnimalPosition);
    const bool lit = lights_beam_on() || lights_glow_on();
    const float sleep = wild_for(animal).sleep;
    sight *= night_sight_share(sqrtf(squared_distance(place, g_hunter)), darkness, daytime_light_level(), lit) * (1.0f - sleep);
    smell *= 1.0f - darkness;
    hearing *= 1.0f - sleep * (1.0f - kSleepHearing);
    real(brain, unused);
    sight = kept_sight;
    smell = kept_smell;
    hearing = kept_hearing;
}

void update_hunt() {
    char* player = walking_player();
    if (!player) {
        g_active = false;
        g_birds = nullptr;
        return;
    }
    const auto position = reinterpret_cast<PositionFn>(field<void**>(player, 0)[kMethodPosition / sizeof(void*)]);
    memcpy(g_hunter, position(player, nullptr), sizeof(g_hunter));
    g_now = now_seconds();
    g_active = true;
    leave_prints();
    const float hour = daytime_hour();
    if (hour >= 0.0f) g_hour = static_cast<int>(hour);
    g_lights_draw = daytime_darkness() >= 1.0f && (lights_beam_on() || lights_glow_on());
    g_zoomed = input_is_zoomed();
    g_beam_read = false;
    if (g_zoomed || (g_lights_draw && lights_beam_on())) {
        float view_place[3];
        const bool read = free_hunt_read_view(view_place, g_forward);
        g_zoomed = g_zoomed && read;
        g_beam_read = read && lights_beam_on();
        const float length = sqrtf(g_forward[0] * g_forward[0] + g_forward[2] * g_forward[2]);
        if (length > 0.0f) {
            g_forward[0] /= length;
            g_forward[2] /= length;
        }
    }
    if (g_now >= g_next_report) {
        if (g_next_report > 0.0) {
            unsigned bedded = 0, asleep = 0, lying_up = 0;
            for (const auto& entry : g_wild) {
                bedded += entry.second.bedded ? 1 : 0;
                asleep += entry.second.sleep >= 1.0f ? 1 : 0;
                lying_up += entry.second.lying_up ? 1 : 0;
            }
            logf("wildlife: %u bedded down, %u asleep, %u wounded lying up, %u lines to run picked, %u cornered, %u ran into you, %u calls",
                 bedded, asleep, lying_up, g_lines_picked, g_cornered, g_barged, g_calls);
            unsigned trailing = 0;
            for (const auto& entry : g_wild) trailing += entry.second.on_blood ? 1 : 0;
            logf("wildlife: fastest %s at %.1f m/s, %u jinks, %u bluff charges, %u chases given up", g_fastest_kind ? g_fastest_kind : "nothing",
                 g_fastest, g_jinks, g_bluffs, g_gave_up);
            logf("wildlife: %u drops of blood, %u on a blood trail, %u of your prints, %u goats headed back up", g_drops, trailing, g_hunter_prints, g_gone_home);
            g_gone_home = 0;
            logf("wildlife: %u went round in circles%s%s, %u sent somewhere else, %u ran at you without attacking%s%s, %u took their own line from the herd's",
                 g_circled, g_circled ? ", latest " : "", g_circled ? g_circle_note : "", g_unstuck, g_at_you, g_at_you ? ", latest " : "",
                 g_at_you ? g_at_you_note : "", g_own_lines);
            g_unstuck = 0;
            g_lines_picked = g_cornered = g_barged = g_calls = g_jinks = g_bluffs = g_gave_up = g_drops = g_hunter_prints = 0;
            g_circled = g_at_you = g_own_lines = 0;
            g_fastest = 0.0f;
            g_fastest_kind = nullptr;
            logf("wildlife: %u ticks run and %u saved at %d:00, %u chases kept quiet, %u drawn to a light, %u scared off by you, %u led to the water's edge", g_ticks_run, g_ticks_saved, g_hour, g_music_kept_quiet, g_drawn, g_scared, g_led_to_shore);
        }
        g_ticks_run = g_ticks_saved = g_music_kept_quiet = g_drawn = g_scared = g_led_to_shore = 0;
        g_next_report = g_now + kReportSeconds;
    }

    char* manager = reinterpret_cast<ManagerFn>(g_game + kAnimalManager)();
    if (!manager) return;
    if (!g_areas_read) read_water(manager);
    if (g_birds != field<char*>(manager, kManagerBirds)) g_birds = nullptr;
    check_bumps(manager);
    if (g_now >= g_next_carcass_look) {
        g_next_carcass_look = g_now + kCarcassLookSeconds;
        look_for_carcasses(manager);
    }
    if (!g_birds) return;

    calm_birds();
    if (g_now >= g_next_look) {
        const float moved = sqrtf(squared_distance(g_hunter, g_last_look_place));
        const bool first = g_next_look == 0.0;
        g_next_look = g_now + kLookSeconds;
        memcpy(g_last_look_place, g_hunter, sizeof(g_last_look_place));
        if (!first && moved / static_cast<float>(kLookSeconds) >= kBigMoveSpeed) startle_birds();
    }
    if (g_shot_heard || g_alarm_nearby) startle_birds();
    g_shot_heard = g_alarm_nearby = false;
}

bool update_hunt_safely() {
    __try {
        update_hunt();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool settle_safely(void* animals, char* definitions) {
    __try {
        settle(animals, definitions);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool stands_to_drink(const Kind* kind) {
    if (!kind) return false;
    for (const char* name : kNoDrinkingPose) {
        if (!strcmp(kind->name, name)) return true;
    }
    return false;
}

bool wildlife_high_ground(const char* species, const float* hunter, float nearest, float* place) {
    if (!wildlife_available() || !species || !climbs(kind_named(species, strlen(species)))) return false;
    float high[3];
    if (!high_ground_safely(place, kClimbArriveReach, hunter, nearest, high)) return false;
    memcpy(place, high, sizeof(high));
    return true;
}

bool wildlife_available() { return g_game != nullptr && g_cfg.free_hunt_wildlife; }

std::string wildlife_species_text(const std::string& original) {
    std::string text = original;
    for (const Kind& kind : kKinds) {
        const std::string opening = std::string("def ") + kind.name + ":/species_kind";
        const size_t from = text.find(opening);
        if (from == std::string::npos) continue;
        size_t to = text.find(":/species_kind", from + opening.size());
        if (to == std::string::npos) to = text.size();
        set_key(&text, from, to, "freehunt_counts", counts_text(kind));
        to = text.find(":/species_kind", from + opening.size());
        if (to == std::string::npos) to = text.size();
        set_key(&text, from, to, "maximal_herd_size", std::to_string(herd_setting(kind.largest)));
    }
    replace_every(&text, ":0.3:food\"", ":0.9:food\"");  // carcasses, and how much each hunter wants one
    set_type_sound(&text, "wolf", "male", kWolfHowl);
    set_type_sound(&text, "wolf", "female", kLongWolfHowl);
    set_type_sound(&text, "wolf", "pup", kLongWolfHowl);
    replace_every(&text, "/sounds/animals/an_rd_male.ogg", "/sounds/animals/an_el_bull.ogg");  // which don't exist
    scale_key(&text, 0, text.size(), "spoor_dist", kPrintGapShare);
    replace_every(&text, "/sounds/animals/an_rd_female.ogg", "/sounds/animals/an_el_cow.ogg");
    return text;
}

std::string wildlife_base_text(const std::string& original) {
    std::string text = original;
    replace_every(&text, "effect_range:\"400\"", "effect_range:\"900\"");
    replace_every(&text, kGamePrintLimits, kPrintLimits);
    return text;
}

std::string wildlife_birds_text(const std::string& original) {
    std::string text = original;
    replace_every(&text, "spawn_frequency_seconds: 7.0f", "spawn_frequency_seconds: 1.5f");
    replace_every(&text, "max_simultaneous_animals: 5", "max_simultaneous_animals: 24");
    replace_every(&text, "min_spawn_distance: 10.0f", "min_spawn_distance: 20.0f");
    replace_every(&text, "max_spawn_distance: 20.0f", "max_spawn_distance: 60.0f");
    return text;
}

std::string wildlife_sticker_text(const std::string& original) {
    std::string text = original;
    replace_every(&text, kGameBloodLimits, kBloodLimits);
    for (size_t at = text.find(":/trail_base {"); at != std::string::npos; at = text.find(":/trail_base {", at + 1)) {
        const size_t start = text.rfind("def ", at);
        const size_t end = text.find('}', at);
        if (start == std::string::npos || end == std::string::npos) break;
        const bool blood = text.substr(start, at - start).find("blood") != std::string::npos;
        scale_key(&text, at, end, "size", blood ? kBloodSize : kPrintSize);
    }
    return text;
}

std::string wildlife_blood_material_text(const std::string& original) {
    std::string text = original;
    replace_every(&text, kOverBlood, kStainedBlood);
    return text == original ? std::string() : text;
}

// Only blood uses this state, now, and nothing used it before.
std::string wildlife_blood_state_text(const std::string& original) {
    if (original.find(kGameMultiply) == std::string::npos) return std::string();
    std::string text = original;
    replace_every(&text, kGameMultiply, kTwiceMultiply);
    const char* line = original.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    text += std::string(line) + "alpha" + line + "{" + line + "\tfunc : gequal" + line + "\tref : 0.5" + line + "}" + line;
    return text;
}

void wildlife_whole_herd(bool whole) { g_whole_herd = whole; }

int wildlife_herd_size(const char* species, int short_of) {
    const Kind* kind = kind_named(species, strlen(species));
    if (!kind) return 0;
    const float value = roll();
    const int size = 1 + static_cast<int>(value * value * kind->largest);
    const int most = short_of < kind->largest ? short_of : kind->largest;
    return size < most ? size : most;
}

int wildlife_herd_group(int size) { return size + 2; }

bool wildlife_near_water(const float* place) {
    for (size_t i = 0; i + 2 < g_water.size(); i += 3) {
        if (squared_distance(place, &g_water[i]) < kNearWater * kNearWater) return true;
    }
    return false;
}

void wildlife_settle_herds(void* animals, char* definitions) {
    if (!wildlife_available()) return;
    if (!settle_safely(animals, definitions)) logf("wildlife: couldn't settle the herds");
}

void wildlife_hunt_started(int hour) {
    g_wild.clear();
    g_water.clear();
    g_carcasses.clear();
    g_escapes.clear();
    memset(g_bullets, 0, sizeof(g_bullets));
    memset(g_blood_spots, 0, sizeof(g_blood_spots));
    g_spooked_notes = 0;
    g_hunter_trail = -2;
    g_last_print_time = 0.0;
    memset(g_alarms, 0, sizeof(g_alarms));
    g_on_hunt = true;
    g_active = false;
    g_lights_draw = false;
    g_areas_read = false;
    g_birds = nullptr;
    g_hour = hour;
    g_next_look = 0.0;
    g_next_carcass_look = 0.0;
    g_startled_until = 0.0;
    g_vultures_called = false;
}

void wildlife_hunt_ended() {
    g_on_hunt = false;
    g_active = false;
    g_birds = nullptr;
}

void wildlife_end_frame() {
    if (!g_on_hunt || !wildlife_available() || !view_hunt_running()) return;
    if (update_hunt_safely()) return;
    g_on_hunt = g_active = false;
    logf("wildlife: something went wrong, so animals go back to normal for this hunt");
}

void wildlife_before_needs(char* brain, WildNeeds* needs) {
    needs->changed = false;
    needs->hid_hurt = false;
    if (!g_active) return;
    char* animal = field<char*>(brain, kBrainAnimal);
    g_thinking = animal;
    Wild& wild = wild_for(animal);
    const float* place = reinterpret_cast<float*>(animal + kAnimalPosition);
    const bool hunts = wild.kind && wild.kind->hunts;
    const bool hurt = field<bool>(animal, kAnimalHurt);
    const double since = wild.last_think > 0.0 ? fmin(g_now - wild.last_think, kLongestThought) : 0.0;
    wild.last_think = g_now;
    char* leader = field<char*>(animal, kAnimalMate);
    if (leader == animal) leader = nullptr;
    if (leader) {
        Wild& followed = wild_for(leader);
        followed.follower_seen = g_now;
        if (wild.young || (wild.kind && wild.kind->family)) {
            followed.young_seen = g_now;
            memcpy(followed.young_place, place, sizeof(followed.young_place));
        }
    }
    if (wild.chase_music_held) {
        const float distance = sqrtf(squared_distance(place, g_hunter));
        if (field<int>(brain, kBrainGoal) != kAttackHunterGoal) {
            wild.chase_music_held = false;
        } else if (!chase_music_waits(kHunterAttackedMusic, distance)) {
            wild.chase_music_held = false;
            logf("wildlife: music %d played, it's %.0f m away now", kHunterAttackedMusic, distance);
            g_real_music(g_music_hunt, nullptr, kHunterAttackedMusic, kChaseMusicVolume);
        }
    }
    const bool just_hurt = hurt && !wild.hurt_seen;
    if (just_hurt) {
        wild.hurt_seen = true;
        raise_alarm(place, kDeathAlarm, g_now);
    }
    bool woken = hurt;
    if (!hunts) woken = catch_panic(brain, wild, place) || woken;
    if (near_miss(wild, place)) {
        woken = true;
        float& fear = field<float>(brain, kBrainHunterFear);
        fear = fmaxf(fear, kNearMissFear);
        if (hunts) wild.provoked_until = g_now + kProvokedSeconds;
    }
    const bool lured = g_lights_draw && wild.sleep < kAwake && follow_light(brain, wild, place);

    needs->hunger = field<float>(brain, kBrainHunger);
    needs->thirst = field<float>(brain, kBrainThirst);
    needs->tiredness = field<float>(brain, kBrainTiredness);
    needs->prey = field<float>(brain, kBrainPrey);
    needs->hunter_fear = field<float>(brain, kBrainHunterFear);
    needs->enemy_fear = field<float>(brain, kBrainEnemyFear);
    needs->lure = field<float>(brain, kBrainLure);
    needs->aggression = field<float>(brain, kBrainAggression);

    const DayPart part = day_part(g_hour);
    float hunger_share = 1.0f, rest_share = 1.0f;
    time_of_day(part, hunts, &hunger_share, &rest_share);
    float hunger = needs->hunger * hunger_share;
    float thirst = needs->thirst;
    float tiredness = needs->tiredness * rest_share;
    float prey = needs->prey;
    float hunter_fear = needs->hunter_fear;
    float enemy_fear = needs->enemy_fear;
    float lure = needs->lure;
    const float distance = sqrtf(squared_distance(place, g_hunter));
    float aggression = distance > kChargeReach ? 0.0f : needs->aggression;

    if (hunts) {
        const bool shot_at = g_now - g_last_shot < kShotSeconds && squared_distance(place, g_shot_place) < kShotReach * kShotReach;
        const bool noticed = distance < kNoticeReach &&
                             (field<float>(brain, kBrainSight) >= kSeesYou || field<float>(brain, kBrainHearing) >= kFlushHears);
        if (field<bool>(animal, kAnimalHurt) || shot_at || distance < kTooClose || noticed) {
            wild.provoked_until = g_now + kProvokedSeconds;
        }
    }
    // Drinking beats anything but real danger, so being frightened has to call it off.
    if (frightened_off_water(hunts, hunter_fear, enemy_fear, g_now < wild.provoked_until)) {
        wild.water_shy_until = g_now + kWaterShySeconds;
    }

    // Asleep, a little noise brings it round slowly, a shot depending on how close, and anything
    // that frightens it at once.
    const bool sleeper = daytime_darkness() >= 1.0f && !hunts && !hurt;
    const bool calm = fmaxf(hunter_fear, enemy_fear) < kCalm;
    float shot_wake = 0.0f;
    if (g_last_shot > wild.last_shot_heard) {
        wild.last_shot_heard = g_last_shot;
        shot_wake = shot_wakes(sqrtf(squared_distance(place, g_shot_place)));
    }
    const float noise = field<float>(brain, kBrainHearing) * kNoiseWakes * static_cast<float>(since);
    wild.sleep = woken || !calm || !sleeper ? 0.0f : fmaxf(0.0f, wild.sleep - noise - shot_wake);
    if (!sleeper) wild.has_bed = wild.bedded = false;
    bool to_bed = false;
    if (sleeper && calm && !lured && !wild.bedded) {
        if (!wild.has_bed) choose_bed(animal, leader, wild, place);
        if (squared_distance(place, wild.bed) <= kBedArrive * kBedArrive) wild.bedded = true;
        else to_bed = true;
    }
    const bool sentinel = g_now - wild.follower_seen < kSentinelSeconds;
    const bool in_bed = sleeper && wild.bedded;
    if (in_bed) {
        tiredness = fmaxf(tiredness, kBedTired);
        if (!sentinel && calm && field<int>(brain, kBrainGoal) == kRestGoal) {
            wild.sleep = fminf(1.0f, wild.sleep + static_cast<float>(since / kFallAsleepSeconds));
        }
    }

    double every = 0.0, length = 0.0;
    drink_times(part, hunts, &every, &length);
    if (stands_to_drink(wild.kind)) length = fmin(length, kStandingDrinkFor);
    if (climbs(wild.kind)) every *= kClimberDrinksLess;
    const bool thirsty = !g_water.empty() && !hurt && !to_bed && !in_bed && g_now >= wild.water_shy_until &&
                         in_window(g_now, wild.phase, every, length);
    if (thirsty) {
        thirst = fmaxf(thirst, kWantsDrink);
        hunger = 0.0f;
        prey = 0.0f;
        tiredness = 0.0f;
    }
    if (hunts) {
        if (thirsty) wild.peace_until = g_now + kWanderSeconds;
        if (g_now < wild.peace_until && g_now >= wild.provoked_until) {
            hunger = 0.0f;
            prey = 0.0f;
            hunter_fear = 0.0f;
            enemy_fear = 0.0f;
        }
    }
    // A wounded animal has the game's hurt flag hidden while it picks, or that'd be all it does.
    if (hurt) {
        if (wild.wound_run <= 0.0f) wild.wound_run = kWoundRunShortest + (kWoundRunLongest - kWoundRunShortest) * own_roll(animal);
        const bool noticed = distance < kFlushReach || field<float>(brain, kBrainSight) >= kSeesYou ||
                             field<float>(brain, kBrainHearing) >= kFlushHears;
        if (wild.lying_up && noticed) {
            wild.lying_up = false;
            wild.wound_run = distance + kFlushRunsOn;
        }
        if (!wild.lying_up && distance >= wild.wound_run) wild.lying_up = true;
        if (wild.lying_up && !wild.was_lying_up) {
            drop_blood(animal, kPoolStain, kPoolDrops);
            wild.next_pool = g_now + kPoolGrowSeconds;
        } else if (wild.lying_up && g_now >= wild.next_pool) {
            drop_blood(animal, kPoolStain, kPoolGrows);
            wild.next_pool = g_now + kPoolGrowSeconds;
        }
        wild.was_lying_up = wild.lying_up;
        if (field<bool>(animal, kAnimalBleeds) && g_now >= wild.next_blood_mark) {
            wild.next_blood_mark = g_now + kBloodSpotGap;
            mark_blood(place);
        }
        if (wild.lying_up) {
            hunter_fear = 0.0f;
            enemy_fear = 0.0f;
            hunger = 0.0f;
            prey = 0.0f;
            tiredness = fmaxf(tiredness, kBedTired);
        } else {
            hunter_fear = fmaxf(hunter_fear, kWoundFear);
        }
    }
    bool defending = false;
    if (wild.kind && wild.kind->family && !leader && g_now - wild.young_seen < kYoungSeenSeconds) {
        const float near_young = fminf(distance, sqrtf(squared_distance(wild.young_place, g_hunter)));
        if (near_young < kDefendReach) {
            defending = true;
            hunter_fear = fmaxf(hunter_fear, kDefendFear);
            aggression = 1.0f;
            wild.provoked_until = g_now + kProvokedSeconds;
            // Whatever she was doing, running away included, is over, so she picks again, and attacks.
            if (field<int>(brain, kBrainGoal) != kAttackHunterGoal) field<int>(brain, kBrainGoalStage) = kGoalDone;
        }
    }
    // Spent, a hunter gives up on whatever it was after.
    const int goal = field<int>(brain, kBrainGoal);
    if (hunts && wild.stamina <= kSpent && g_now >= wild.gave_up_until && (goal == kAttackHunterGoal || goal == kHuntPreyGoal)) {
        wild.gave_up_until = g_now + kGiveUpSeconds;
        field<int>(brain, kBrainGoalStage) = kGoalDone;
        ++g_gave_up;
    }
    if (g_now < wild.gave_up_until) {
        aggression = 0.0f;
        prey = 0.0f;
    }
    // Something agile cuts off to the side as what's after it closes in, and back again after.
    const bool fleeing = (goal == kFleeGoal || goal == kEnemyGoal) && field<int>(animal, kAnimalMoveMode) == kFleeMode;
    if (wild.gait && wild.gait->agile && fleeing) {
        float threat[3];
        if (g_now >= wild.next_jink && threat_place(animal, threat) && squared_distance(place, threat) < kJinkReach * kJinkReach) {
            wild.jink_until = g_now + kJinkShortest + (kJinkLongest - kJinkShortest) * roll();
            wild.next_jink = g_now + kJinkGapShortest + (kJinkGapLongest - kJinkGapShortest) * roll();
            wild.jink_side = roll() < 0.5f ? -1.0f : 1.0f;
            wild.jinking = true;
            field<int>(brain, kBrainGoalStage) = kGoalDone;
            ++g_jinks;
        } else if (wild.jinking && g_now >= wild.jink_until) {
            wild.jinking = false;
            field<int>(brain, kBrainGoalStage) = kGoalDone;
        }
    }
    // Some charges stop short, or veer away, and it leaves you alone for a while.
    if (goal == kAttackHunterGoal && !wild.charging) {
        wild.charging = true;
        wild.bluffing = wild.gait && roll() < wild.gait->bluff;
        wild.bluff_at = kBluffNearest + (kBluffFurthest - kBluffNearest) * roll();
    } else if (goal != kAttackHunterGoal) {
        wild.charging = false;
    }
    if (wild.charging && wild.bluffing && distance < wild.bluff_at) {
        wild.bluffing = false;
        wild.bluffed_until = g_now + kBluffSeconds;
        wild.bluff_veers = roll() < 0.5f;
        field<int>(brain, kBrainGoalStage) = kGoalDone;
        ++g_bluffs;
    }
    if (g_now < wild.bluffed_until) {
        aggression = 0.0f;
        hunter_fear = wild.bluff_veers ? fmaxf(hunter_fear, kBluffRunsFear) : 0.0f;
    }
    if (g_now < wild.cornered_until && distance < kCornerReach && needs->aggression > 0.0f) {
        aggression = 1.0f;
        if (field<int>(brain, kBrainGoal) == kFleeGoal) field<int>(brain, kBrainGoalStage) = kGoalDone;
    }
    if (wild.voice) {
        const Voice& voice = *wild.voice;
        Wild& herd = leader ? wild_for(leader) : wild;
        if (just_hurt && !voice.howl) {
            call_out(animal, false);
        } else if (voice.alarm && field<float>(brain, kBrainHunterFear) >= kAlarmFear && g_now - herd.herd_alarm_at > kHerdAlarmGap) {
            herd.herd_alarm_at = g_now;
            call_out(animal, false);
        } else if (voice.growl && (goal == kAttackHunterGoal || defending) && g_now - wild.growled_at > kGrowlGap) {
            wild.growled_at = g_now;
            call_out(animal, false);
        } else if (voice.howl && calm && daytime_darkness() >= 1.0f) {
            if (!leader) {
                if (wild.next_call == 0.0) {
                    wild.next_call = g_now + kHowlEvery * roll();
                } else if (g_now >= wild.next_call) {
                    wild.next_call = g_now + kHowlEvery * (0.5 + roll());
                    wild.howled_at = g_now;
                    call_out(animal, true);
                }
            } else if (herd.howled_at > wild.joined_howl && g_now - herd.howled_at >= own_roll(animal) * kHowlJoins) {
                wild.joined_howl = herd.howled_at;
                call_out(animal, true);
            }
        } else if (voice.every > 0.0f && calm && wild.sleep < kAwake && field<int>(brain, kBrainNeed) != kDangerNeed) {
            const float gap = voice.rut && wild.male && part == DayPart::Peak ? voice.every / kDawnCalls : voice.every;
            if (wild.next_call == 0.0) {
                wild.next_call = g_now + gap * roll();
            } else if (g_now >= wild.next_call) {
                wild.next_call = g_now + gap * (0.5 + roll());
                if (distance < kCallReach) call_out(animal, true);
            }
        }
    }
    // A hunter that isn't busy follows fresh blood, until it's close enough to find what's bleeding.
    const float* scent = nullptr;
    if (hunts && !hurt && !thirsty && calm && g_now >= wild.gave_up_until && goal != kAttackHunterGoal && goal != kHuntPreyGoal) {
        scent = blood_scent(place);
    }
    wild.on_blood = scent != nullptr;
    // A climber that's wandered off its heights goes back up. Anywhere at least as high is home now.
    bool to_home = false;
    if (wild.has_home && calm && !thirsty && !hurt && !lured && !to_bed && !in_bed && !scent) {
        if (place[1] >= wild.home[1]) memcpy(wild.home, place, sizeof(wild.home));
        const float x = place[0] - wild.home[0], z = place[2] - wild.home[2];
        to_home = x * x + z * z > kClimberRoam * kClimberRoam;
        if (to_home && !wild.going_home) ++g_gone_home;
    }
    wild.going_home = to_home;
    const float* lure_place = lured ? g_hunter : to_bed ? wild.bed : to_home ? wild.home : scent;
    if (lure_place) {
        lure = fmaxf(lure, scent && !lured && !to_bed && !to_home ? kBloodPull : kLightPull);
        memcpy(animal + kAnimalLurePlace, lure_place, sizeof(g_hunter));
        if (to_bed) tiredness = 0.0f;
        if (field<int>(brain, kBrainGoal) == kLureGoal && g_now >= wild.next_lure_walk) {
            wild.next_lure_walk = g_now + kLureWalkGap;
            field<int>(brain, kBrainGoalStage) = 0;
        }
    }
    field<float>(brain, kBrainHunger) = hunger;
    field<float>(brain, kBrainThirst) = thirst;
    field<float>(brain, kBrainTiredness) = tiredness;
    field<float>(brain, kBrainPrey) = prey;
    field<float>(brain, kBrainHunterFear) = hunter_fear;
    field<float>(brain, kBrainEnemyFear) = enemy_fear;
    field<float>(brain, kBrainLure) = lure;
    field<float>(brain, kBrainAggression) = aggression;
    needs->used_hunger = hunger;
    needs->used_thirst = thirst;
    needs->used_tiredness = tiredness;
    needs->used_prey = prey;
    needs->used_hunter_fear = hunter_fear;
    needs->used_enemy_fear = enemy_fear;
    needs->used_lure = lure;
    needs->used_aggression = aggression;
    if (hurt) {
        field<bool>(animal, kAnimalHurt) = false;
        needs->hid_hurt = true;
    }
    needs->changed = true;
}

void wildlife_after_needs(char* brain, const WildNeeds& needs) {
    g_thinking = nullptr;
    if (needs.hid_hurt) field<bool>(field<char*>(brain, kBrainAnimal), kAnimalHurt) = true;
    if (!needs.changed) return;
    field<float>(brain, kBrainHunger) += needs.hunger - needs.used_hunger;
    field<float>(brain, kBrainThirst) += needs.thirst - needs.used_thirst;
    field<float>(brain, kBrainTiredness) += needs.tiredness - needs.used_tiredness;
    field<float>(brain, kBrainPrey) += needs.prey - needs.used_prey;
    field<float>(brain, kBrainHunterFear) += needs.hunter_fear - needs.used_hunter_fear;
    field<float>(brain, kBrainEnemyFear) += needs.enemy_fear - needs.used_enemy_fear;
    field<float>(brain, kBrainLure) += needs.lure - needs.used_lure;
    field<float>(brain, kBrainAggression) += needs.aggression - needs.used_aggression;
}

void wildlife_attach_game(HMODULE game, bool verified) {
    if (!verified) return;
    g_game = reinterpret_cast<char*>(game);

    const void* no_impact = g_game + kNoImpact;
    const void* dirt_impact = g_game + kDirtImpact;
    const bool tree = patch_bytes(g_game + kImpactTable + (kTreeSurface - kFirstImpactSurface) * sizeof(void*), &no_impact, &dirt_impact, sizeof(void*));

    const unsigned char game_limit = kGameMostAnimals;
    const unsigned char our_limit = kMostAnimals;
    const float* forty = reinterpret_cast<const float*>(g_game + kFortyFloat);
    const float* ours = &kMostAnimalsFloat;
    const bool limit = patch_bytes(g_game + kMostAnimalsCheck, &game_limit, &our_limit, sizeof(our_limit)) &&
                       patch_bytes(g_game + kTrimTotal, &kGameMostAnimals, &kMostAnimals, sizeof(kMostAnimals)) &&
                       patch_bytes(g_game + kTrimScale, &forty, &ours, sizeof(ours));

    g_group_roll = reinterpret_cast<void*>(&hook_group_roll);
    const void* random = g_game + kRandom;
    const void* group_roll = &g_group_roll;
    const bool groups = patch_bytes(g_game + kGroupRollCall, &random, &group_roll, sizeof(random));
    const bool fire = patch_call(g_game + kFireCall, g_game + kFire, reinterpret_cast<const void*>(&hook_fire));
    bool ticks = true;
    for (const uintptr_t slot : kTickSlots) {
        ticks = vtable_patch(reinterpret_cast<void**>(g_game + slot), reinterpret_cast<void*>(&hook_animal_tick), reinterpret_cast<void**>(&g_real_tick)) && ticks;
    }
    const bool music = vtable_patch(reinterpret_cast<void**>(g_game + kMusicSlot), reinterpret_cast<void*>(&hook_music), reinterpret_cast<void**>(&g_real_music));
    const bool senses = patch_call(g_game + kSensesCall, g_game + kSenses, reinterpret_cast<const void*>(&hook_senses));
    const float* game_reach = reinterpret_cast<const float*>(g_game + kTwoHundredSquared);
    const float* our_reach = &kCallReachSquared;
    const bool calls = patch_bytes(g_game + kCallHeardWithin, &game_reach, &our_reach, sizeof(our_reach)) &&
                       patch_bytes(g_game + kCallFadesBy, &kGameCallReach, &kCallReach, sizeof(kCallReach));
    g_real_slerp = *reinterpret_cast<SlerpFn*>(g_game + kSlerpImport);
    g_turn_slot = reinterpret_cast<void*>(&hook_turn);
    const void* game_slerp = g_game + kSlerpImport;
    const void* our_slerp = &g_turn_slot;
    const bool gaits = g_real_slerp && patch_bytes(g_game + kTurnCall, &game_slerp, &our_slerp, sizeof(our_slerp)) &&
                       patch_call(g_game + kRootMotionCall, g_game + kRootMotion, reinterpret_cast<const void*>(&hook_root_motion));
    const float* game_ten = reinterpret_cast<const float*>(g_game + kTen);
    const float* our_drip = &kDripEvery;
    const bool blood = patch_call(g_game + kDripCall, g_game + kDropBlood, reinterpret_cast<const void*>(&hook_drip)) &&
                       patch_call(g_game + kHitBloodCall, g_game + kDropBlood, reinterpret_cast<const void*>(&hook_hit_blood)) &&
                       patch_bytes(g_game + kDripEveryUse, &game_ten, &our_drip, sizeof(our_drip)) &&
                       patch_call(g_game + kStampCall, g_game + kStamp, reinterpret_cast<const void*>(&hook_stamp));
    const float* game_charge = reinterpret_cast<const float*>(g_game + kGameChargeFrom);
    const float* our_charge = &kChargeFrom;
    const bool charge = patch_bytes(g_game + kChargeFromUse, &game_charge, &our_charge, sizeof(our_charge));
    bool flee = true;
    for (const uintptr_t call : kFleeTargetCalls) {
        flee = patch_call(g_game + call, g_game + kFleeTarget, reinterpret_cast<const void*>(&hook_flee_target)) && flee;
    }
    const bool water = patch_call(g_game + kBulletRayCall, g_game + kRayCast, reinterpret_cast<const void*>(&hook_bullet_ray)) &&
                       patch_call(g_game + kWaterTargetCall, g_game + kAreaTarget, reinterpret_cast<const void*>(&hook_water_target));
    logf("wildlife: tree puff=%d limit=%d groups=%d shots=%d ticks=%d music=%d senses=%d water=%d flee=%d calls=%d gaits=%d blood=%d charge=%d", tree, limit, groups, fire, ticks, music, senses, water, flee, calls, gaits, blood, charge);
}

int wildlife_test() {
    for (const Kind& kind : kKinds) {
        const int setting = herd_setting(kind.largest);
        if (biggest_group(setting, 0.9999f) != kind.largest) return 1;        // the generator can reach it, and no further
        if (kind.largest > kMostInAnyHerd) return 2;
        if (kind.low >= kind.medium || kind.medium >= kind.high) return 3;
    }
    const Kind& deer = kKinds[0];
    const std::string counts = counts_text(deer);
    if (counts.find("14,24,40,1,2,") != 0 || counts.find(",15") == std::string::npos) return 4;
    if (wildlife_herd_group(1) != 3) return 5;  // the first entry past the three densities

    const std::string game =
        "def species {\r\n\tdef wtail:/species_kind {\r\n\t\tname:\"a\"\r\n\t\tfreehunt_counts:\"2,3,5\"\r\n\t\tdef carcass:/carcass_base {\r\n"
        "\t\t\tattract00:\"grizzly:0.3:food\"\r\n\t\t\trepell00:\"all:0.2:food\"\r\n\t\t}\r\n\t}\r\n\tdef grizzly:/species_kind {\r\n"
        "\t\tmaximal_herd_size:\"2\"\r\n\t}\r\n}\r\n";
    const std::string changed = wildlife_species_text(game);
    if (changed.find("freehunt_counts:\"14,24,40,1,") == std::string::npos) return 6;
    if (changed.find("2,3,5") != std::string::npos) return 7;
    if (changed.find("maximal_herd_size:\"16\"") == std::string::npos) return 8;   // added where it wasn't set
    if (changed.find("maximal_herd_size:\"4\"") == std::string::npos) return 9;    // and replaced where it was
    if (changed.find("maximal_herd_size:\"2\"") != std::string::npos) return 10;
    if (changed.find("grizzly:0.9:food") == std::string::npos) return 11;
    if (changed.find("all:0.2:food") == std::string::npos) return 12;              // repelling is left alone
    if (changed.find("freehunt_counts:\"4,7,12") >= changed.find("wtail") && changed.find("def grizzly") > changed.find("freehunt_counts:\"4,7,12")) return 13;
    if (wildlife_base_text("effect_range:\"400\"") != "effect_range:\"900\"") return 14;

    const std::string birds = wildlife_birds_text("  spawn_frequency_seconds: 7.0f\r\n  max_simultaneous_animals: 5\r\n  min_spawn_distance: 10.0f\r\n  max_spawn_distance: 20.0f\r\n");
    if (birds.find("max_simultaneous_animals: 24") == std::string::npos || birds.find("max_spawn_distance: 60.0f") == std::string::npos) return 15;
    if (birds.find("spawn_frequency_seconds: 1.5f") == std::string::npos || birds.find("min_spawn_distance: 20.0f") == std::string::npos) return 16;

    if (!in_window(0.0, 0.0f, 360.0, 180.0) || in_window(200.0, 0.0f, 360.0, 180.0)) return 17;
    if (in_window(0.0, 0.5f, 360.0, 180.0) || !in_window(180.0, 0.5f, 360.0, 180.0)) return 18;  // a later turn
    double every = 0.0, length = 0.0;
    drink_times(day_part(7), false, &every, &length);
    if (length != kPeakDrinkFor) return 19;
    drink_times(day_part(12), false, &every, &length);
    if (every != kQuietDrinkEvery || length != kQuietDrinkFor) return 20;
    float hunger = 0.0f, rest = 0.0f;
    time_of_day(day_part(18), false, &hunger, &rest);
    if (hunger != kPeakHunger || rest != kPeakRest) return 21;
    time_of_day(day_part(12), false, &hunger, &rest);
    if (hunger != kQuietHunger || rest != kQuietRest) return 22;
    time_of_day(day_part(23), false, &hunger, &rest);
    if (hunger != kNightHunger || rest != kNightRest) return 29;
    time_of_day(day_part(2), true, &hunger, &rest);
    if (hunger != kProwlHunger || rest != kProwlRest) return 30;     // hunters are up when the rest aren't
    drink_times(day_part(23), false, &every, &length);
    if (every != kQuietDrinkEvery) return 31;
    drink_times(day_part(23), true, &every, &length);
    if (every != kDrinkEvery) return 32;
    if (day_part(10) != DayPart::Day || day_part(15) != DayPart::Day) return 33;
    if (day_part(21) != DayPart::Night || day_part(4) != DayPart::Night || day_part(5) != DayPart::Peak) return 34;

    if (tick_gap(50.0f, false) != 0.0f) return 35;                   // close up, every frame
    if (tick_gap(200.0f, false) != kNearGap) return 36;
    if (tick_gap(450.0f, false) != kFarGap) return 37;
    if (tick_gap(900.0f, false) != kFurthestGap) return 38;
    if (tick_gap(900.0f, true) != kNearGap) return 39;               // unless a scope is on it
    if (tick_gap(50.0f, true) != 0.0f) return 40;

    if (music_plays(kAnimalAttacksMusic, true) || music_plays(kAnimalAttacksMusic, false)) return 41;  // a chase that isn't after you stays quiet
    if (!music_plays(2, true)) return 42;                            // one that is doesn't
    if (music_plays(kTargetNearMusic, true)) return 43;              // nor does something nearby in a free hunt
    if (!music_plays(kTargetNearMusic, false)) return 54;            // though missions still get that hint
    if (!music_plays(4, true)) return 55;                            // and nothing else is touched
    if (!chase_music_waits(kHunterAttackedMusic, 200.0f)) return 56;  // the chase waits till it's close
    if (chase_music_waits(kHunterAttackedMusic, 50.0f)) return 57;
    if (chase_music_waits(4, 200.0f)) return 58;

    if (!frightened_off_water(false, 0.5f, 0.0f, false) || !frightened_off_water(false, 0.0f, 0.5f, false)) return 59;  // fear ends a drink
    if (frightened_off_water(false, 0.1f, 0.1f, false)) return 60;   // a little doesn't
    if (frightened_off_water(true, 0.9f, 0.9f, false)) return 61;    // a hunter's fear is what sets it off
    if (!frightened_off_water(true, 0.0f, 0.0f, true)) return 62;    // so it goes by being provoked

    if (flee_angle(0) != 0.0f || flee_angle(1) != kFleeAngleStep || flee_angle(2) != -kFleeAngleStep) return 63;  // straight away first
    if (flee_angle(kFleeAngles - 1) != -(kFleeAngles / 2) * kFleeAngleStep) return 64;
    if (shot_wakes(50.0f) != 1.0f || shot_wakes(kShotWakeReach) != 0.0f) return 65;  // a shot nearby wakes it, one far off doesn't
    if (shot_wakes(300.0f) <= 0.0f || shot_wakes(300.0f) >= 1.0f) return 66;
    const float from[3] = {0.0f, 0.0f, 0.0f}, to[3] = {100.0f, 0.0f, 0.0f};
    const float beside[3] = {50.0f, 0.0f, 5.0f}, beyond[3] = {130.0f, 0.0f, 0.0f};
    if (fabsf(segment_distance(beside, from, to) - 5.0f) > 1e-4f || fabsf(segment_distance(beyond, from, to) - 30.0f) > 1e-4f) return 67;
    if (clamped_climb(60.0f) != 1.0f || clamped_climb(-15.0f) != -0.5f) return 68;
    if (own_roll(reinterpret_cast<char*>(0x1000)) < 0.0f || own_roll(reinterpret_cast<char*>(0x1000)) >= 1.0f) return 69;

    if (!male_name("sp12_wtail_buck00") || !male_name("elk_bull03") || !male_name("turkey_gobbler01")) return 70;
    if (male_name("wtail_doe03") || male_name("wolf_female01") || male_name(nullptr)) return 71;  // female has male in it
    if (!voice_of(&kKinds[0]) || voice_of(&kKinds[0])->every <= 0.0f || voice_of(kind_of_animal("cougar_tom00"))) return 72;
    for (const Voice& voice : kVoices) {
        if (!kind_named(voice.kind, strlen(voice.kind))) return 73;                // every voice is a kind
    }
    const std::string wolves = wildlife_species_text(
        "def wolf:/species_kind {\r\n\tdef male:/species_type {\r\n\t\ts_specific:\"/sounds/animals/an_missing.ogg\"\r\n\t}\r\n"
        "\tdef female:/species_type {\r\n\t\ts_specific:\"/sounds/animals/an_missing.ogg\"\r\n\t}\r\n}\r\n"
        "def reddeer:/species_kind {\r\n\ts_specific:\"/sounds/animals/an_rd_male.ogg\"\r\n}\r\n");
    if (wolves.find(kWolfHowl) == std::string::npos || wolves.find(kLongWolfHowl) == std::string::npos) return 74;  // wolves howl
    if (wolves.find("an_missing") != std::string::npos || wolves.find("an_rd_") != std::string::npos) return 75;

    for (const Kind& kind : kKinds) {
        const Gait* gait = gait_of(&kind);
        if (!gait) return 76;                                           // every kind runs
        if (gait->top < gait->run || gait->turn < 1.0f || gait->stamina <= 0.0f) return 77;  // and never slower than it did
    }
    float legs = 0.0f, stride = 0.0f;
    pace_shares(5.0f, 10.0f, &legs, &stride);
    if (legs != 2.0f || stride != 1.0f) return 78;                      // the legs go first
    pace_shares(3.5f, 12.0f, &legs, &stride);
    if (legs != kMostLegs || fabsf(stride - 12.0f / 3.5f / kMostLegs) > 1e-4f) return 79;  // then the stride
    pace_shares(3.0f, 30.0f, &legs, &stride);
    if (stride != kMostStride) return 80;                               // up to a point
    pace_shares(6.0f, 4.0f, &legs, &stride);
    if (legs != 1.0f || stride != 1.0f) return 81;                      // and never slower
    if (tired_share(1.0f) != 1.0f || tired_share(kTiredFrom) != 1.0f || tired_share(0.0f) != kTiredPace) return 82;
    if (tired_share(kTiredFrom / 2) <= kTiredPace || tired_share(kTiredFrom / 2) >= 1.0f) return 83;
    if (!young_name("sp3_grizzly_cub00") || !young_name("wolf_pup01") || young_name("wtail_buck02")) return 84;

    if (drops_for(0.0f) != 1 || drops_for(2.5f) != 3 || drops_for(40.0f) != kMostDrops) return 85;  // the harder it bleeds, the more
    const float hunter_at[3] = {0.0f, 0.0f, 0.0f};
    const float near_side[3] = {0.0f, 0.0f, 20.0f}, far_side[3] = {0.0f, 0.0f, -20.0f};
    const float herd_line[2] = {0.0f, 1.0f};
    if (line_runs_towards(near_side, hunter_at, herd_line) || !line_runs_towards(far_side, hunter_at, herd_line)) return 93;  // not through you
    if (fabsf(turned_by(3.0f, -3.0f) - 0.2832f) > 0.001f || fabsf(turned_by(0.5f, 1.0f) - 0.5f) > 0.001f) return 94;  // the short way round
    if (!stands_to_drink(kind_named("grizzly", 7)) || stands_to_drink(kind_named("wtail", 5)) || stands_to_drink(nullptr)) return 95;
    if (!climbs(kind_named("goat", 4)) || climbs(kind_named("bighorn", 7)) || climbs(nullptr)) return 96;  // mountain goats climb
    const std::string stickers = wildlife_sticker_text(
        "def blood_trail:/trail_base {\r\n\tlimits:\"50:200:400\"\r\n\tsize00:\"0.17\"\r\n\tsize01:\"0.25\"\r\n}\r\n"
        "def wtail_trail:/trail_base {\r\n\tsize:\"0.12\"\r\n}\r\n");
    if (stickers.find(kBloodLimits) == std::string::npos) return 86;                    // more blood kept
    if (stickers.find("size00:\"0.272\"") == std::string::npos || stickers.find("size01:\"0.400\"") == std::string::npos) return 87;  // bigger
    if (stickers.find("size:\"0.204\"") == std::string::npos) return 88;                  // and bigger prints
    if (wildlife_blood_material_text("material : \"dry.tex.fog.nc.decal.over\" {").find(kStainedBlood) == std::string::npos) return 89;
    const std::string state = wildlife_blood_state_text("blend\r\n{\r\n\tsrc : dst_color\r\n\tdst : zero\r\n}\r\n");
    if (state.find(kTwiceMultiply) == std::string::npos || state.find("gequal") == std::string::npos) return 90;
    if (!wildlife_blood_state_text("blend { src : one }").empty()) return 91;              // and only that state
    std::string gaps = "spoor_dist:\"2.00\" spoor_dist:\"0.35\"";
    scale_key(&gaps, 0, gaps.size(), "spoor_dist", kPrintGapShare);
    if (gaps != "spoor_dist:\"1.200\" spoor_dist:\"0.210\"") return 92;

    const float calm[kBirdKinds] = {0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.0f};
    float chances[kBirdKinds];
    favour_bird(chances, calm, kBirdKinds, kVulture, kVultureShare);
    if (fabsf(chances[kBirdKinds - 1] - 1.0f) > 1e-5f) return 23;                  // it still adds up
    if (chances[kVulture] - chances[kVulture - 1] < kVultureShare) return 24;      // and vultures got their share
    for (int i = 1; i < kBirdKinds; ++i) {
        if (chances[i] <= chances[i - 1]) return 25;
    }
    if (kind_of_animal("sp12_wtail_buck00") != &kKinds[0] || kind_of_animal("wtail_doe03") != &kKinds[0]) return 26;
    if (kind_of_animal("sp3_grizzly_cub00") == nullptr || !kind_of_animal("sp3_grizzly_cub00")->hunts) return 27;
    if (kind_of_animal("nothing") != nullptr) return 28;

    if (night_sight_share(50.0f, 0.0f, 1.0f, false) != 1.0f) return 44;           // by day nothing changes
    if (night_sight_share(50.0f, 1.0f, 0.1f, true) != 0.0f) return 45;            // at night it's short
    if (fabsf(night_sight_share(10.0f, 1.0f, 0.1f, false) - 10.0f) > 1e-4f) return 46;  // but as good as by day up close
    if (night_sight_share(25.0f, 1.0f, 0.1f, false) != 0.0f) return 47;
    if (night_sight_share(25.0f, 1.0f, 0.1f, true) == 0.0f) return 48;            // and a light shows you up
    const float way[3] = {1.0f, 0.0f, 0.0f};
    if (!light_noticed(0.0f, 90.0f, true, false, way) || !light_noticed(-90.0f, 0.0f, true, false, way)) return 49;  // a lantern, all around
    if (light_noticed(0.0f, 110.0f, true, false, way)) return 50;
    if (!light_noticed(140.0f, 0.0f, false, true, way)) return 51;                // a beam goes further
    if (light_noticed(0.0f, 140.0f, false, true, way) || light_noticed(-20.0f, 0.0f, false, true, way)) return 52;  // but only where it's pointing
    if (light_noticed(160.0f, 0.0f, false, true, way) || light_noticed(10.0f, 0.0f, false, false, way)) return 53;
    return 0;
}

}  // namespace longshot
