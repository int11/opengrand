/* Program change table (committed source; originally generated from the owner's manual program change list) — edit by hand. */
#include "ca79ng/router.h"

/* On2 (GM2) map: {GM2 program 1..128, bank MSB, bank LSB, CA79 sound index or -1 (GM2-only voice)} */
const progmap_entry_t PROGMAP_ON2[] = {
    {1, 95, 112, 0}, /* SK-EX Rendering */
    {1, 121, 0, 1}, /* SK-EX | Concert */
    {1, 121, 1, 2}, /* SK-EX | Jazz Clean */
    {1, 121, 2, 3}, /* SK-EX | Warm */
    {1, 95, 27, 4}, /* EX | Concert */
    {1, 95, 29, 5}, /* EX | Warm */
    {1, 95, 16, 6}, /* EX | Standard */
    {1, 95, 28, 7}, /* EX | Pop Grand */
    {2, 95, 10, 8}, /* EX | Pop Piano */
    {2, 121, 0, 9}, /* EX | Modern */
    {1, 95, 30, 10}, /* SK-5 | Clasical */
    {1, 95, 31, 11}, /* SK-5 | Pop */
    {1, 95, 25, 12}, /* UprightPiano | Clasical */
    {5, 121, 0, 13}, /* Classic Electric Piano */
    {5, 121, 3, 14}, /* 60’s Electric Piano */
    {6, 121, 0, 15}, /* Modern Electric Piano */
    {5, 121, 1, 16}, /* Classic Electric Piano 2 */
    {5, 95, 5, 17}, /* Classic Electric Piano 3 */
    {6, 95, 1, 18}, /* Crystal Electric Piano */
    {18, 121, 0, 19}, /* Jazz Organ */
    {17, 121, 0, 20}, /* Blues Organ */
    {17, 95, 5, 21}, /* Ballad Organ */
    {17, 95, 3, 22}, /* Gospel Organ */
    {17, 95, 1, 23}, /* Drawbar Organ */
    {17, 95, 2, 24}, /* Drawbar Organ 2 */
    {20, 121, 0, 25}, /* Church Organ */
    {20, 95, 7, 26}, /* Diapason */
    {21, 95, 1, 27}, /* Full Ensemble */
    {20, 95, 6, 28}, /* Diapason Octave */
    {20, 95, 17, 29}, /* Chiffy Tibia */
    {20, 95, 24, 30}, /* Principal Octave */
    {20, 95, 41, 31}, /* Soft Strings */
    {20, 95, 48, 32}, /* Mellow Flutes */
    {20, 95, 42, 33}, /* Medium Ensemble */
    {20, 95, 43, 34}, /* Loud Ensembe */
    {20, 95, 47, 35}, /* Bright Ensemble */
    {20, 95, 44, 36}, /* Full Organ */
    {7, 121, 0, 37}, /* Harpsichord */
    {7, 121, 1, 38}, /* Harpsichord Octave */
    {8, 121, 0, 39}, /* Vibraphone */
    {13, 121, 0, 40}, /* Clavi */
    {9, 95, 1, 41}, /* Marimba */
    {7, 121, 3, 42}, /* Celesta */
    {45, 95, 1, 43}, /* Slow Strings */
    {49, 95, 8, 44}, /* String Pad */
    {49, 95, 1, 45}, /* Warm Strings */
    {49, 121, 0, 46}, /* String Ensemble */
    {50, 95, 1, 47}, /* Soft Orchestra */
    {47, 121, 0, 48}, /* Harp */
    {53, 121, 0, 49}, /* Choir */
    {54, 95, 39, 50}, /* Pop Ooh */
    {54, 95, 40, 51}, /* Pop Aah */
    {54, 95, 53, 52}, /* Choir 2 */
    {54, 95, 2, 53}, /* Jazz Ensemble */
    {54, 95, 7, 54}, /* Pop Ensemble */
    {89, 121, 0, 55}, /* New Age Pad */
    {90, 95, 5, 56}, /* Square Pad */
    {92, 121, 1, 57}, /* Itopia */
    {101, 95, 1, 58}, /* Brightness */
    {62, 95, 2, 59}, /* Brass Pad */
    {93, 121, 0, 60}, /* Bowed Pad */
    {33, 121, 0, 61}, /* Wood Bass */
    {34, 121, 0, 62}, /* Electric Bass */
    {36, 121, 0, 63}, /* Electric Bass 2 */
    {33, 95, 1, 64}, /* Wood Bass & Ride */
    {26, 95, 6, 65}, /* Ballad Guitar */
    {25, 95, 3, 66}, /* Pick Nylon Guitar */
    {2, 121, 1, -1}, /* Rock Piano */
    {3, 121, 1, -1}, /* Electric Grand2 */
    {4, 121, 1, -1}, /* Wide Honky Tonk */
    {5, 121, 2, -1}, /* Classic E.P. 5 */
    {6, 121, 3, -1}, /* Legend E.P. */
    {6, 121, 4, -1}, /* Phase E.P. */
    {7, 121, 2, -1}, /* WideHarpsichord */
    {8, 121, 1, -1}, /* Synth Clavi */
    {9, 121, 0, -1}, /* Celesta */
    {10, 121, 0, -1}, /* Glockenspiel */
    {11, 121, 0, -1}, /* Music Box */
    {12, 121, 1, -1}, /* Wide Vibraphone */
    {13, 121, 1, -1}, /* Wide Marimba */
    {14, 121, 0, -1}, /* Xylophone */
    {15, 121, 0, -1}, /* Tubular Bells */
    {15, 121, 1, -1}, /* Church Bells */
    {15, 121, 2, -1}, /* Carillon */
    {16, 121, 0, -1}, /* Dulcimer */
    {17, 121, 1, -1}, /* Drawbar Organ 5 */
    {17, 121, 2, -1}, /* 60’s Organ */
    {18, 121, 1, -1}, /* Perc. Organ */
    {19, 121, 0, -1}, /* Rock Organ */
    {20, 121, 1, -1}, /* Church Organ 2 */
    {20, 121, 2, -1}, /* Church Organ 3 */
    {21, 121, 0, -1}, /* Reed Organ */
    {21, 121, 1, -1}, /* Puff Organ */
    {22, 121, 0, -1}, /* FrenchAccordion */
    {22, 121, 1, -1}, /* Accordion */
    {23, 121, 0, -1}, /* Harmonica */
    {24, 121, 0, -1}, /* Tango Accordion */
    {25, 121, 0, -1}, /* Nylon Acoustic */
    {25, 121, 1, -1}, /* Ukulele */
    {25, 121, 2, -1}, /* Nylon Acoustic2 */
    {25, 121, 3, -1}, /* Nylon Acoustic3 */
    {26, 121, 0, -1}, /* Steel Guitar */
    {26, 121, 1, -1}, /* 12 String */
    {26, 121, 2, -1}, /* Mandolin */
    {26, 121, 3, -1}, /* Steel Guitar 2 */
    {27, 121, 0, -1}, /* Jazz Guitar */
    {27, 121, 1, -1}, /* Pedal Steel */
    {28, 121, 0, -1}, /* Electric Guitar */
    {28, 121, 1, -1}, /* E. Guitar 2 */
    {28, 121, 2, -1}, /* Rhythm Guitar */
    {29, 121, 0, -1}, /* Muted Electric */
    {29, 121, 1, -1}, /* Cutting Guitar2 */
    {29, 121, 2, -1}, /* E. Guitar 3 */
    {29, 121, 3, -1}, /* Country Lead */
    {30, 121, 0, -1}, /* OverdriveGuitar */
    {30, 121, 1, -1}, /* Dynmic Ov.drive */
    {31, 121, 0, -1}, /* Distortion */
    {31, 121, 1, -1}, /* Dist Feedback */
    {31, 121, 2, -1}, /* Dist Rhythm */
    {32, 121, 0, -1}, /* E.Gtr Harmonics */
    {32, 121, 1, -1}, /* Guitar Feedback */
    {34, 121, 1, -1}, /* FingerSlap Bass */
    {35, 121, 0, -1}, /* Pick Bass */
    {37, 121, 0, -1}, /* Slap Bass */
    {38, 121, 0, -1}, /* Slap Bass 2 */
    {39, 121, 0, -1}, /* Synth Bass */
    {39, 121, 1, -1}, /* Warm Synth Bass */
    {39, 121, 2, -1}, /* Synth Bass 3 */
    {39, 121, 3, -1}, /* Clavi Bass */
    {39, 121, 4, -1}, /* Hammer Bass */
    {40, 121, 0, -1}, /* Synth Bass 2 */
    {40, 121, 1, -1}, /* Synth Bass 4 */
    {40, 121, 2, -1}, /* Rubber Bass */
    {40, 121, 3, -1}, /* Attack Bass */
    {41, 121, 0, -1}, /* Violin */
    {41, 121, 1, -1}, /* Slow Violin */
    {42, 121, 0, -1}, /* Viola */
    {43, 121, 0, -1}, /* Cello */
    {44, 121, 0, -1}, /* Contrabass */
    {45, 121, 0, -1}, /* Tremolo Strings */
    {47, 121, 1, -1}, /* Celtic Harp */
    {48, 121, 0, -1}, /* Timpani */
    {49, 121, 1, -1}, /* Strings & Brass */
    {49, 121, 2, -1}, /* 60’s Strings */
    {50, 121, 0, -1}, /* StringEnsemble2 */
    {51, 121, 0, -1}, /* Synth Strings */
    {51, 121, 1, -1}, /* Synth Strings 3 */
    {52, 121, 0, -1}, /* Synth Strings 2 */
    {53, 121, 1, -1}, /* Choir 3 */
    {54, 121, 0, -1}, /* Voice Oohs */
    {54, 121, 1, -1}, /* Humming */
    {55, 121, 0, -1}, /* Synth Vocal */
    {55, 121, 1, -1}, /* Analog Voice */
    {56, 121, 0, -1}, /* Orchestra Hit */
    {56, 121, 1, -1}, /* Bass Hit Plus */
    {56, 121, 2, -1}, /* 6th Hit */
    {56, 121, 3, -1}, /* Euro Hit */
    {57, 121, 0, -1}, /* Trumpet */
    {57, 121, 1, -1}, /* Solo Trumpet */
    {58, 121, 0, -1}, /* Trombone */
    {58, 121, 1, -1}, /* Trombone 2 */
    {58, 121, 2, -1}, /* Bright Trombone */
    {59, 121, 0, -1}, /* Tuba */
    {60, 121, 0, -1}, /* Muted Trumpet */
    {60, 121, 1, -1}, /* Muted Trumpet 2 */
    {61, 121, 0, -1}, /* French Horns */
    {61, 121, 1, -1}, /* Warm FrenchHorn */
    {62, 121, 0, -1}, /* Brass Section */
    {62, 121, 1, -1}, /* Brass Section 2 */
    {63, 121, 0, -1}, /* Synth Brass */
    {63, 121, 1, -1}, /* Synth Brass 3 */
    {63, 121, 2, -1}, /* Analog Brass */
    {63, 121, 3, -1}, /* Jump Brass */
    {64, 121, 0, -1}, /* Synth Brass 2 */
    {64, 121, 1, -1}, /* Synth Brass 4 */
    {64, 121, 2, -1}, /* Analog Brass 2 */
    {65, 121, 0, -1}, /* Soprano Sax */
    {66, 121, 0, -1}, /* Alto Sax */
    {67, 121, 0, -1}, /* Tenor Sax */
    {68, 121, 0, -1}, /* Baritone Sax */
    {69, 121, 0, -1}, /* Oboe */
    {70, 121, 0, -1}, /* English Horn */
    {71, 121, 0, -1}, /* Bassoon */
    {72, 121, 0, -1}, /* Clarinet */
    {73, 121, 0, -1}, /* Piccolo */
    {74, 121, 0, -1}, /* Flute */
    {75, 121, 0, -1}, /* Recorder */
    {76, 121, 0, -1}, /* Pan Flute */
    {77, 121, 0, -1}, /* Blown Bottle */
    {78, 121, 0, -1}, /* Shakuhachi */
    {79, 121, 0, -1}, /* Whistle */
    {80, 121, 0, -1}, /* Ocarina */
    {81, 121, 0, -1}, /* Square */
    {81, 121, 1, -1}, /* Square 2 */
    {81, 121, 2, -1}, /* Sine */
    {82, 121, 0, -1}, /* Classic Synth */
    {82, 121, 1, -1}, /* Classic Synth 2 */
    {82, 121, 2, -1}, /* Lead */
    {82, 121, 3, -1}, /* Classic Synth 3 */
    {82, 121, 4, -1}, /* SequencedAnalog */
    {83, 121, 0, -1}, /* Caliope */
    {84, 121, 0, -1}, /* Chiff */
    {85, 121, 0, -1}, /* Charang */
    {85, 121, 1, -1}, /* Wire Lead */
    {86, 121, 0, -1}, /* Voice */
    {87, 121, 0, -1}, /* Fifth */
    {88, 121, 0, -1}, /* Bass & Lead */
    {88, 121, 1, -1}, /* Soft Wire Lead */
    {90, 121, 0, -1}, /* Warm Pad */
    {90, 121, 1, -1}, /* Sine Pad */
    {91, 121, 0, -1}, /* Polysynth */
    {92, 121, 0, -1}, /* Choir Pad */
    {94, 121, 0, -1}, /* Metallic Pad */
    {95, 121, 0, -1}, /* Halo Pad */
    {96, 121, 0, -1}, /* Sweep Pad */
    {97, 121, 0, -1}, /* Rain Pad */
    {98, 121, 0, -1}, /* Soundtrack */
    {99, 121, 0, -1}, /* Crystal */
    {99, 121, 1, -1}, /* Synth Mallet */
    {100, 121, 0, -1}, /* Atmosphere */
    {101, 121, 0, -1}, /* Brightness */
    {102, 121, 0, -1}, /* Goblin */
    {103, 121, 0, -1}, /* Echoes */
    {103, 121, 1, -1}, /* Echo Bell */
    {103, 121, 2, -1}, /* Echo Pan */
    {104, 121, 0, -1}, /* Sci-Fi */
    {105, 121, 0, -1}, /* Sitar */
    {105, 121, 1, -1}, /* Sitar 2 */
    {106, 121, 0, -1}, /* Banjo */
    {107, 121, 0, -1}, /* Shamisen */
    {108, 121, 0, -1}, /* Koto */
    {108, 121, 1, -1}, /* Taisho Koto */
    {109, 121, 0, -1}, /* Kalimba */
    {110, 121, 0, -1}, /* Bag Pipe */
    {111, 121, 0, -1}, /* Fiddle */
    {112, 121, 0, -1}, /* Shanai */
    {113, 121, 0, -1}, /* Tinkle Bell */
    {114, 121, 0, -1}, /* Agogo */
    {115, 121, 0, -1}, /* Steel Drums */
    {116, 121, 0, -1}, /* Woodblock */
    {116, 121, 1, -1}, /* Castanet */
    {117, 121, 0, -1}, /* Taiko Drums */
    {117, 121, 1, -1}, /* Concert BD */
    {118, 121, 0, -1}, /* Melodic Toms */
    {118, 121, 1, -1}, /* Melodic Toms 2 */
    {119, 121, 0, -1}, /* Synth Drum */
    {119, 121, 1, -1}, /* Rhythm Box Tom */
    {119, 121, 2, -1}, /* Electric Drum */
    {120, 121, 0, -1}, /* Reverse Cymbal */
    {121, 121, 0, -1}, /* Gtr Fret Noise */
    {121, 121, 1, -1}, /* GtrCuttingNoise */
    {121, 121, 2, -1}, /* Ac Bass Slap */
    {122, 121, 0, -1}, /* Breath Noise */
    {122, 121, 1, -1}, /* Flute Key Click */
    {123, 121, 0, -1}, /* Seashore */
    {123, 121, 1, -1}, /* Rain */
    {123, 121, 2, -1}, /* Thunder */
    {123, 121, 3, -1}, /* Wind */
    {123, 121, 4, -1}, /* Stream */
    {123, 121, 5, -1}, /* Bubble */
    {124, 121, 0, -1}, /* Bird Tweet */
    {124, 121, 1, -1}, /* Dog Barking */
    {124, 121, 2, -1}, /* Horse Gallop */
    {124, 121, 3, -1}, /* Bird Tweet 2 */
    {125, 121, 0, -1}, /* Telephone */
    {125, 121, 1, -1}, /* Telephone 2 */
    {125, 121, 2, -1}, /* Door Creak */
    {125, 121, 3, -1}, /* Door Slam */
    {125, 121, 4, -1}, /* Scratch */
    {125, 121, 5, -1}, /* Wind Chime */
    {126, 121, 0, -1}, /* Helicopter */
    {126, 121, 1, -1}, /* Car Engine */
    {126, 121, 2, -1}, /* Car Stopping */
    {126, 121, 3, -1}, /* Car Passing */
    {126, 121, 4, -1}, /* Car Crash */
    {126, 121, 5, -1}, /* Siren */
    {126, 121, 6, -1}, /* Train */
    {126, 121, 7, -1}, /* Jet Plane */
    {126, 121, 8, -1}, /* Starship */
    {126, 121, 9, -1}, /* Burst Noise */
    {127, 121, 0, -1}, /* Applause */
    {127, 121, 1, -1}, /* Laughing */
    {127, 121, 2, -1}, /* Screaming */
    {127, 121, 3, -1}, /* Punch */
    {127, 121, 4, -1}, /* Heartbeat */
    {127, 121, 5, -1}, /* Foot Step */
    {128, 121, 0, -1}, /* Gunshot */
    {128, 121, 1, -1}, /* Machine Gun */
    {128, 121, 2, -1}, /* Laser Gun */
    {128, 121, 3, -1}, /* Explosion */
    {1, 120, 0, -1}, /* StereoPop Set */
    {33, 120, 0, -1}, /* StereoAmbien */
    {9, 120, 0, -1}, /* Stereo Ballad */
    {26, 120, 0, -1}, /* Analog Set */
    {27, 120, 0, -1}, /* CP Dance */
    {17, 120, 0, -1}, /* Power Set */
    {25, 120, 0, -1}, /* Electronic Set */
    {41, 120, 0, -1}, /* Brush Set */
    {49, 120, 0, -1}, /* Orchestra Set */
    {57, 120, 0, -1}, /* SFX Set */
};
const unsigned PROGMAP_ON2_COUNT = 300;
