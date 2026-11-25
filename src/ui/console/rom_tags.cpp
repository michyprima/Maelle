/*
    Copyright 2026 michyprima

    This file is part of Maelle.

    Maelle is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Maelle is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Maelle. If not, see <https://www.gnu.org/licenses/>.
*/

#include <cstdio>
#include <utility>

#include "rom_tags.h"

// Two-letter maker codes for GBA (and DS and GameCube), from Dolphin's GetCompanyFromID (Source/Core/DiscIO/Enums.cpp)
static const std::pair<const char*, const char*> makerCodes[] = {
    { "01", "Nintendo" },
    { "02", "Nintendo" },
    { "08", "Capcom" },
    { "0A", "Jaleco" },
    { "0L", "Warashi" },
    { "0M", "Entertainment Software Publishing" },
    { "0Q", "IE Institute" },
    { "13", "Electronic Arts Japan" },
    { "18", "Hudson Soft" },
    { "1K", "Titus Software" },
    { "20", "DSI Games" },
    { "28", "Kemco Japan" },
    { "29", "SETA Corporation" },
    { "2K", "NEC Interchannel" },
    { "2L", "Agatsuma Entertainment" },
    { "2M", "Jorudan" },
    { "2N", "Smilesoft" },
    { "2Q", "MediaKite" },
    { "36", "Codemasters" },
    { "41", "Ubisoft" },
    { "4F", "Eidos Interactive" },
    { "4Q", "Disney Interactive Studios" },
    { "4Z", "Crave Entertainment" },
    { "51", "Acclaim Entertainment" },
    { "52", "Activision" },
    { "54", "Take-Two Interactive" },
    { "5D", "Midway Games" },
    { "5G", "Majesco Entertainment" },
    { "5H", "3DO" },
    { "5L", "NewKidCo" },
    { "5S", "Evolved Games" },
    { "5V", "Agetec" },
    { "5Z", "Data Design" },
    { "60", "Titus Interactive" },
    { "64", "LucasArts" },
    { "68", "Bethesda Softworks" },
    { "69", "Electronic Arts" },
    { "6E", "Sega" },
    { "6K", "UFO Interactive Games" },
    { "6L", "BAM! Entertainment" },
    { "6M", "System 3" },
    { "6N", "Midas Interactive Entertainment" },
    { "6S", "TDK Mediactive" },
    { "6U", "The Adventure Company" },
    { "6V", "JoWooD Entertainment" },
    { "6W", "Sega" },
    { "6X", "Wanadoo Edition" },
    { "6Z", "NDS Software" },
    { "70", "Atari (Infogrames)" },
    { "71", "Interplay Entertainment" },
    { "75", "SCi Games" },
    { "78", "THQ" },
    { "7D", "Sierra Entertainment" },
    { "7F", "Kemco" },
    { "7G", "Rage Software" },
    { "7H", "Encore Software" },
    { "7J", "Zushi Games" },
    { "7K", "Kiddinx Entertainment" },
    { "7L", "Simon & Schuster Interactive" },
    { "7M", "Badland Games" },
    { "7N", "Empire Interactive" },
    { "7S", "Rockstar Games" },
    { "7T", "Scholastic" },
    { "7U", "Ignition Entertainment" },
    { "82", "Namco" },
    { "8G", "NEC Interchannel" },
    { "8J", "Kadokawa Shoten" },
    { "8M", "CyberFront" },
    { "8N", "Success" },
    { "8P", "Sega" },
    { "91", "Chunsoft" },
    { "99", "Marvelous Entertainment" },
    { "9B", "Tecmo" },
    { "9G", "Take-Two Interactive" },
    { "9S", "Brother International" },
    { "9Z", "Crunchyroll" },
    { "A4", "Konami" },
    { "A7", "Takara" },
    { "AF", "Namco Bandai Games" },
    { "AU", "Alternative Software" },
    { "AX", "Vivendi" },
    { "B0", "Acclaim Japan" },
    { "B2", "Bandai Games" },
    { "BB", "Sunsoft" },
    { "BL", "MTO" },
    { "BM", "XING" },
    { "BN", "Sunrise Interactive" },
    { "BP", "Global A Entertainment" },
    { "C0", "Taito" },
    { "C8", "Koei" },
    { "CM", "Konami Computer Entertainment Osaka" },
    { "CQ", "From Software" },
    { "D9", "Banpresto" },
    { "DA", "Tomy" },
    { "DQ", "Compile Heart" },
    { "E5", "Epoch" },
    { "E6", "Game Arts" },
    { "E7", "Athena" },
    { "E8", "Asmik Ace Entertainment" },
    { "E9", "Natsume" },
    { "EB", "Atlus" },
    { "EL", "Spike" },
    { "EM", "Konami Computer Entertainment Tokyo" },
    { "EP", "Sting Entertainment" },
    { "ES", "Starfish-SD" },
    { "EY", "Vblank Entertainment" },
    { "FH", "Easy Interactive" },
    { "FJ", "Virtual Toys" },
    { "FK", "The Game Factory" },
    { "FP", "Mastiff" },
    { "FR", "Digital Tainment Pool" },
    { "FS", "XS Games" },
    { "G0", "Alpha Unit" },
    { "G2", "Yuke's" },
    { "G6", "SIMS" },
    { "G9", "D3 Publisher" },
    { "GA", "PIN Change" },
    { "GD", "Square Enix" },
    { "GE", "Kids Station" },
    { "GG", "O3 Entertainment" },
    { "GJ", "Detn8 Games" },
    { "GK", "Genki" },
    { "GL", "Gameloft" },
    { "GM", "Gamecock Media Group" },
    { "GN", "Oxygen Games" },
    { "GR", "GSP" },
    { "GT", "505 Games" },
    { "GX", "Commodore" },
    { "GY", "The Game Factory" },
    { "GZ", "Gammick Entertainment" },
    { "H3", "Zen United" },
    { "H4", "SNK Playmore" },
    { "HA", "Nobilis" },
    { "HE", "Gust" },
    { "HF", "Level-5" },
    { "HG", "Graffiti Entertainment" },
    { "HH", "Focus Home Interactive" },
    { "HJ", "Genius Products" },
    { "HK", "D2C Games" },
    { "HL", "Frontier Developments" },
    { "HM", "HMH Interactive" },
    { "HN", "High Voltage Software" },
    { "HQ", "Abstraction Games" },
    { "HS", "Tru Blu" },
    { "HT", "Big Blue Bubble" },
    { "HU", "Ghostfire Games" },
    { "HW", "Incredible Technologies" },
    { "HY", "Reef Entertainment" },
    { "HZ", "Nordcurrent" },
    { "J8", "D4 Enterprise" },
    { "J9", "AQ Interactive" },
    { "JD", "SKONEC Entertainment" },
    { "JE", "E Frontier" },
    { "JF", "Arc System Works" },
    { "JG", "The Games Company" },
    { "JH", "City Interactive" },
    { "JJ", "Deep Silver" },
    { "JP", "redspotgames" },
    { "JR", "Engine Software" },
    { "JS", "Digital Leisure" },
    { "JT", "Empty Clip Studios" },
    { "JU", "Riverman Media" },
    { "JV", "JV Games" },
    { "JW", "BigBen Interactive" },
    { "JX", "Shin'en Multimedia" },
    { "JY", "Steel Penny Games" },
    { "JZ", "505 Games" },
    { "K2", "Coca-Cola (Japan) Company" },
    { "K3", "Yudo" },
    { "K6", "Nihon System" },
    { "KB", "Nippon Ichi Software" },
    { "KG", "Kando Games" },
    { "KH", "Joju Games" },
    { "KJ", "Studio Zan" },
    { "KK", "DK Games" },
    { "KL", "Abylight" },
    { "KM", "Deep Silver" },
    { "KN", "Gameshastra" },
    { "KP", "Purple Hills" },
    { "KQ", "Over the Top Games" },
    { "KR", "KREA Medie" },
    { "KT", "The Code Monkeys" },
    { "KW", "Semnat Studios" },
    { "KY", "Medaverse Studios" },
    { "L3", "G-Mode" },
    { "L8", "FujiSoft" },
    { "LB", "Tryfirst" },
    { "LD", "Studio Zan" },
    { "LF", "Kemco" },
    { "LG", "Black Bean Games" },
    { "LJ", "Legendo Entertainment" },
    { "LL", "HB Studios" },
    { "LN", "GameOn" },
    { "LP", "Left Field Productions" },
    { "LR", "Koch Media" },
    { "LT", "Legacy Interactive" },
    { "LU", "Lexis Num\xc3\xa9rique" },
    { "LW", "Grendel Games" },
    { "LY", "Icon Games" },
    { "M0", "Studio Zan" },
    { "M1", "Grand Prix Games" },
    { "M2", "HomeMedia" },
    { "M4", "Cybird" },
    { "M6", "Perpetuum" },
    { "MB", "Agenda" },
    { "MD", "Ateam" },
    { "ME", "Silver Star Japan" },
    { "MF", "Yamasa" },
    { "MH", "Mentor Interactive" },
    { "MJ", "Mumbo Jumbo" },
    { "ML", "DTP Young Entertainment" },
    { "MM", "Big John Games" },
    { "MN", "Mindscape" },
    { "MR", "Mindscape" },
    { "MS", "Milestone" },
    { "MT", "Blast! Entertainment" },
    { "MV", "Marvelous Entertainment" },
    { "MZ", "Mad Catz" },
    { "N0", "Exkee" },
    { "N4", "Zoom" },
    { "N7", "T&S" },
    { "N9", "Tera Box" },
    { "NA", "Tom Create" },
    { "NB", "HI Games & Publishing" },
    { "NE", "Kosaido" },
    { "NF", "Peakvox" },
    { "NG", "Nordic Games" },
    { "NH", "Gevo Entertainment" },
    { "NJ", "Enjoy Gaming" },
    { "NK", "Neko Entertainment" },
    { "NL", "Nordic Softsales" },
    { "NN", "Nnooo" },
    { "NP", "Nobilis" },
    { "NQ", "Namco Bandai Partners" },
    { "NR", "Destineer Publishing" },
    { "NS", "Nippon Ichi Software America" },
    { "NT", "Nocturnal Entertainment" },
    { "NV", "Nicalis" },
    { "NW", "Deep Fried Entertainment" },
    { "NX", "Barnstorm Games" },
    { "NY", "Nicalis" },
    { "P1", "Poisoft" },
    { "PH", "Playful Entertainment" },
    { "PK", "Knowledge Adventure" },
    { "PL", "Playlogic Entertainment" },
    { "PM", "Warner Bros. Interactive Entertainment" },
    { "PN", "P2 Games" },
    { "PQ", "PopCap Games" },
    { "PS", "Bplus" },
    { "PT", "Firemint" },
    { "PU", "Pub Company" },
    { "PV", "Pan Vision" },
    { "PY", "Playstos Entertainment" },
    { "PZ", "GameMill Publishing" },
    { "Q2", "Santa Entertainment" },
    { "Q3", "Asterizm" },
    { "Q4", "Hamster" },
    { "Q5", "Recom" },
    { "QA", "Miracle Kidz" },
    { "QC", "Kadokawa Shoten" },
    { "QH", "Virtual Play Games" },
    { "QK", "MACHINE Studios" },
    { "QM", "Object Vision Software" },
    { "QQ", "Gamelion" },
    { "QR", "Lapland Studio" },
    { "QT", "CALARIS" },
    { "QU", "QubicGames" },
    { "QV", "Ludia" },
    { "QW", "Kaasa Solution" },
    { "QX", "Press Play" },
    { "QZ", "Hands-On Mobile" },
    { "RA", "Office Create" },
    { "RG", "Ronimo Games" },
    { "RH", "h2f Games" },
    { "RM", "Rondomedia" },
    { "RN", "Mastiff" },
    { "RQ", "GolemLabs & Zoozen" },
    { "RS", "Brash Entertainment" },
    { "RT", "RTL Enterprises" },
    { "RV", "bitComposer Games" },
    { "RW", "RealArcade" },
    { "RX", "Reflexive Entertainment" },
    { "RZ", "Akaoni Studio" },
    { "S5", "SouthPeak Games" },
    { "SH", "Sabarasa" },
    { "SJ", "Cosmonaut Games" },
    { "SP", "Blade Interactive Studios" },
    { "SQ", "Sonalysts" },
    { "SR", "SnapDragon Games" },
    { "SS", "Sanuk Games" },
    { "ST", "Stickmen Studios" },
    { "SU", "Slitherine" },
    { "SV", "SevenOne Intermedia" },
    { "SZ", "Storm City Games" },
    { "TH", "Kolkom" },
    { "TJ", "Broken Rules" },
    { "TL", "Telltale Games" },
    { "TR", "Tetris Online" },
    { "TS", "Triangle Studios" },
    { "TV", "Tivola" },
    { "TW", "Two Tribes" },
    { "TY", "Teyon" },
    { "UG", "Data Design Interactive" },
    { "UH", "Intenium Console" },
    { "UJ", "Ghostlight" },
    { "UK", "iFun4all" },
    { "UN", "Chillingo" },
    { "UP", "EnjoyUp Games" },
    { "UR", "Sudden Games" },
    { "US", "USM" },
    { "UU", "Onteca" },
    { "UV", "Fugazo" },
    { "UW", "Coresoft" },
    { "VG", "Vogster Entertainment" },
    { "VK", "Sandlot Games" },
    { "VL", "Eko Software" },
    { "VN", "Valcon Games" },
    { "VP", "Virgin Play" },
    { "VS", "Korner Entertainment" },
    { "VT", "Microforum Games" },
    { "VU", "Double Jungle" },
    { "VV", "Pixonauts" },
    { "VX", "Frontline Studios" },
    { "VZ", "Little Orbit" },
    { "WD", "Amazon" },
    { "WG", "2D Boy" },
    { "WH", "NinjaBee" },
    { "WJ", "Studio Walljump" },
    { "WL", "Wired Productions" },
    { "WN", "tons of bits" },
    { "WP", "White Park Bay Software" },
    { "WQ", "Revistronic" },
    { "WR", "Warner Bros. Interactive Entertainment" },
    { "WS", "MonkeyPaw Games" },
    { "WW", "Slang Publishing" },
    { "WY", "WayForward Technologies" },
    { "WZ", "Wizarbox" },
    { "X0", "SDP Games" },
    { "X3", "CK Games" },
    { "X4", "Easy Interactive" },
    { "XB", "Hulu" },
    { "XG", "XGen Studios" },
    { "XJ", "XSEED Games" },
    { "XK", "Exkee" },
    { "XM", "DreamBox Games" },
    { "XN", "Netflix" },
    { "XS", "Aksys Games" },
    { "XT", "Funbox Media" },
    { "XU", "Shanblue Interactive" },
    { "XV", "Keystone Game Studio" },
    { "XW", "Lemon Games" },
    { "XY", "Gaijin Games" },
    { "Y1", "Tubby Games" },
    { "Y5", "Easy Interactive" },
    { "Y6", "Motiviti" },
    { "Y7", "The Learning Company" },
    { "Y9", "RadiationBurn" },
    { "YC", "NECA" },
    { "YD", "Infinite Dreams" },
    { "YF", "O2 Games" },
    { "YG", "Maximum Family Games" },
    { "YJ", "Frozen Codebase" },
    { "YK", "MAD Multimedia" },
    { "YN", "Game Factory" },
    { "YS", "Yullaby" },
    { "YT", "Corecell Technology" },
    { "YV", "KnapNok Games" },
    { "YX", "Selectsoft" },
    { "YY", "FDG Entertainment" },
    { "Z4", "Ntreev Soft" },
    { "Z5", "Shinsegae I&C" },
    { "ZA", "WBA Interactive" },
    { "ZG", "Zallag" },
    { "ZH", "Internal Engine" },
    { "ZJ", "Performance Designed Products" },
    { "ZK", "Anima Game Studio" },
    { "ZP", "Fishing Cactus" },
    { "ZS", "Zinkia Entertainment" },
    { "ZV", "RedLynx" },
    { "ZW", "Judo Baby" },
    { "ZX", "TopWare Interactive" },
};

// Game Boy new licensee codes, used when the old one is 0x33, from Pan Docs (The Cartridge Header)
static const std::pair<const char*, const char*> gbNewLicensees[] = {
    { "01", "Nintendo" },
    { "08", "Capcom" },
    { "13", "Electronic Arts" },
    { "18", "Hudson Soft" },
    { "19", "B-AI" },
    { "20", "KSS)" },
    { "22", "Planning Office WADA" },
    { "24", "PCM Complete" },
    { "25", "San-X" },
    { "28", "Kemco" },
    { "29", "SETA Corporation" },
    { "30", "Viacom)" },
    { "31", "Nintendo" },
    { "32", "Bandai" },
    { "33", "Ocean Software/Acclaim Entertainment" },
    { "34", "Konami" },
    { "35", "HectorSoft" },
    { "37", "Taito" },
    { "38", "Hudson Soft" },
    { "39", "Banpresto" },
    { "41", "Ubi Soft" },
    { "42", "Atlus" },
    { "44", "Malibu Interactive" },
    { "46", "Angel" },
    { "47", "Bullet-Proof Software" },
    { "49", "Irem" },
    { "50", "Absolute" },
    { "51", "Acclaim Entertainment" },
    { "52", "Activision" },
    { "53", "Sammy USA Corporation" },
    { "54", "Konami" },
    { "55", "Hi Tech Expressions" },
    { "56", "LJN" },
    { "57", "Matchbox)" },
    { "58", "Mattel" },
    { "59", "Milton Bradley Company" },
    { "60", "Titus Interactive" },
    { "61", "Virgin Games Ltd." },
    { "64", "Lucasfilm Games" },
    { "67", "Ocean Software" },
    { "69", "Electronic Arts" },
    { "70", "Infogrames" },
    { "71", "Interplay Entertainment" },
    { "72", "Broderbund" },
    { "73", "Sculptured Software" },
    { "75", "The Sales Curve Limited" },
    { "78", "THQ" },
    { "79", "Accolade" },
    { "80", "Misawa Entertainment" },
    { "83", "LOZC G." },
    { "86", "Tokuma Shoten" },
    { "87", "Tsukuda Original" },
    { "91", "Chunsoft Co." },
    { "92", "Video System" },
    { "93", "Ocean Software/Acclaim Entertainment" },
    { "95", "Varie" },
    { "96", "Yonezawa/S'Pal" },
    { "97", "Kaneko" },
    { "99", "Pack-In-Video" },
    { "9H", "Bottom Up" },
    { "A4", "Konami" },
    { "BL", "MTO)" },
    { "DK", "Kodansha" },
};

// Game Boy old licensee codes, from Pan Docs (The Cartridge Header)
static const std::pair<uint8_t, const char*> gbOldLicensees[] = {
    { 0x01, "Nintendo" },
    { 0x08, "Capcom" },
    { 0x09, "HOT-B" },
    { 0x0A, "Jaleco" },
    { 0x0B, "Coconuts Japan" },
    { 0x0C, "Elite Systems" },
    { 0x13, "Electronic Arts" },
    { 0x18, "Hudson Soft" },
    { 0x19, "ITC Entertainment" },
    { 0x1A, "Yanoman" },
    { 0x1D, "Japan Clary" },
    { 0x1F, "Virgin Games Ltd." },
    { 0x24, "PCM Complete" },
    { 0x25, "San-X" },
    { 0x28, "Kemco" },
    { 0x29, "SETA Corporation" },
    { 0x30, "Infogrames" },
    { 0x31, "Nintendo" },
    { 0x32, "Bandai" },
    { 0x34, "Konami" },
    { 0x35, "HectorSoft" },
    { 0x38, "Capcom" },
    { 0x39, "Banpresto" },
    { 0x3C, "Entertainment Interactive" },
    { 0x3E, "Gremlin" },
    { 0x41, "Ubi Soft" },
    { 0x42, "Atlus" },
    { 0x44, "Malibu Interactive" },
    { 0x46, "Angel" },
    { 0x47, "Spectrum HoloByte" },
    { 0x49, "Irem" },
    { 0x4A, "Virgin Games Ltd." },
    { 0x4D, "Malibu Interactive" },
    { 0x4F, "U.S. Gold" },
    { 0x50, "Absolute" },
    { 0x51, "Acclaim Entertainment" },
    { 0x52, "Activision" },
    { 0x53, "Sammy USA Corporation" },
    { 0x54, "GameTek" },
    { 0x55, "Park Place" },
    { 0x56, "LJN" },
    { 0x57, "Matchbox)" },
    { 0x59, "Milton Bradley Company" },
    { 0x5A, "Mindscape)" },
    { 0x5B, "Romstar" },
    { 0x5C, "Naxat Soft" },
    { 0x5D, "Tradewest" },
    { 0x60, "Titus Interactive" },
    { 0x61, "Virgin Games Ltd." },
    { 0x67, "Ocean Software" },
    { 0x69, "Electronic Arts" },
    { 0x6E, "Elite Systems" },
    { 0x6F, "Electro Brain" },
    { 0x70, "Infogrames" },
    { 0x71, "Interplay Entertainment" },
    { 0x72, "Broderbund" },
    { 0x73, "Sculptured Software" },
    { 0x75, "The Sales Curve Limited" },
    { 0x78, "THQ" },
    { 0x79, "Accolade" },
    { 0x7A, "Triffix Entertainment" },
    { 0x7C, "MicroProse" },
    { 0x7F, "Kemco" },
    { 0x80, "Misawa Entertainment" },
    { 0x83, "LOZC G." },
    { 0x86, "Tokuma Shoten" },
    { 0x8B, "Bullet-Proof Software" },
    { 0x8C, "Vic Tokai Corp." },
    { 0x8E, "Ape Inc." },
    { 0x8F, "I'Max" },
    { 0x91, "Chunsoft Co." },
    { 0x92, "Video System" },
    { 0x93, "Tsubaraya Productions" },
    { 0x95, "Varie" },
    { 0x96, "Yonezawa/S'Pal" },
    { 0x97, "Kemco" },
    { 0x99, "Arc" },
    { 0x9A, "Nihon Bussan" },
    { 0x9B, "Tecmo" },
    { 0x9C, "Imagineer)" },
    { 0x9D, "Banpresto" },
    { 0x9F, "Nova" },
    { 0xA1, "Hori Electric" },
    { 0xA2, "Bandai" },
    { 0xA4, "Konami" },
    { 0xA6, "Kawada" },
    { 0xA7, "Takara" },
    { 0xA9, "Technos Japan" },
    { 0xAA, "Broderbund" },
    { 0xAC, "Toei Animation" },
    { 0xAD, "Toho" },
    { 0xAF, "Namco" },
    { 0xB0, "Acclaim Entertainment" },
    { 0xB1, "ASCII Corporation or Nexsoft" },
    { 0xB2, "Bandai" },
    { 0xB4, "Square Enix" },
    { 0xB6, "HAL Laboratory" },
    { 0xB7, "SNK" },
    { 0xB9, "Pony Canyon" },
    { 0xBA, "Culture Brain" },
    { 0xBB, "Sunsoft" },
    { 0xBD, "Sony Imagesoft" },
    { 0xBF, "Sammy Corporation" },
    { 0xC0, "Taito" },
    { 0xC2, "Kemco" },
    { 0xC3, "Square)" },
    { 0xC4, "Tokuma Shoten" },
    { 0xC5, "Data East" },
    { 0xC6, "Tonkin House" },
    { 0xC8, "Koei" },
    { 0xC9, "UFL" },
    { 0xCA, "Ultra Games" },
    { 0xCB, "VAP, Inc." },
    { 0xCC, "Use Corporation" },
    { 0xCD, "Meldac" },
    { 0xCE, "Pony Canyon" },
    { 0xCF, "Angel" },
    { 0xD0, "Taito" },
    { 0xD1, "SOFEL (Software Engineering Lab)" },
    { 0xD2, "Quest" },
    { 0xD3, "Sigma Enterprises" },
    { 0xD4, "ASK Kodansha Co." },
    { 0xD6, "Naxat Soft" },
    { 0xD7, "Copya System" },
    { 0xD9, "Banpresto" },
    { 0xDA, "Tomy" },
    { 0xDB, "LJN" },
    { 0xDD, "Nippon Computer Systems" },
    { 0xDE, "Human Ent." },
    { 0xDF, "Altron" },
    { 0xE0, "Jaleco" },
    { 0xE1, "Towa Chiki" },
    { 0xE2, "Yutaka) # Needs more info" },
    { 0xE3, "Varie" },
    { 0xE5, "Epoch" },
    { 0xE7, "Athena)" },
    { 0xE8, "Asmik Ace Entertainment" },
    { 0xE9, "Natsume" },
    { 0xEA, "King Records)" },
    { 0xEB, "Atlus" },
    { 0xEC, "Epic/Sony Records" },
    { 0xEE, "IGS" },
    { 0xF0, "A Wave" },
    { 0xF3, "Extreme Entertainment" },
    { 0xFF, "LJN" },
};


template <typename K, size_t N>
static std::string lookup(const std::pair<K, const char*> (&table)[N], K key) {
    for (const auto &entry : table)
        if (key == entry.first) return entry.second;
    return "";
}

template <size_t N>
static std::string lookup(const std::pair<const char*, const char*> (&table)[N], const std::string &key) {
    for (const auto &entry : table)
        if (key == entry.first) return entry.second;
    return "";
}

static bool printable(const uint8_t *text, size_t length) {
    for (size_t i = 0; i < length; i++)
        if (text[i] < 0x20 || text[i] >= 0x7F) return false;
    return true;
}

std::string RomTags::region(char letter) {
    // Name the destination from a game code's last letter (GBATEK, NDS Gamecodes)
    switch (letter) {
        case 'A': return "Asia";
        case 'C': return "China";
        case 'D': return "Germany";
        case 'E': return "USA";
        case 'F': return "France";
        case 'H': return "Netherlands";
        case 'I': return "Italy";
        case 'J': return "Japan";
        case 'K': return "Korea";
        case 'L': return "USA";
        case 'M': return "Sweden";
        case 'N': return "Norway";
        case 'O': return "International";
        case 'P': return "Europe";
        case 'Q': return "Denmark";
        case 'R': return "Russia";
        case 'S': return "Spain";
        case 'T': return "USA, Australia";
        case 'U': return "Australia";
        case 'V': return "Europe, Australia";
        case 'W': case 'X': case 'Y': case 'Z': return "Europe";
        default: return "";
    }
}

std::string RomTags::size(uint64_t bytes) {
    // Give a file's size in KB under 1 MB, with one decimal under 10 MB, and in whole MB above
    char text[32];
    if (bytes < (1 << 20))
        snprintf(text, sizeof(text), "%u KB", unsigned((bytes + 1023) >> 10));
    else if (bytes < (10 << 20))
        snprintf(text, sizeof(text), "%.1f MB", bytes / 1048576.0);
    else
        snprintf(text, sizeof(text), "%u MB", unsigned((bytes + (1 << 19)) >> 20));
    return text;
}

std::vector<std::string> RomTags::gba(const uint8_t *header, size_t length) {
    // Only trust a header with its fixed value and a readable game code (GBATEK, GBA Cartridge Header)
    std::vector<std::string> tags = { "Game Boy Advance" };
    if (length < 0xC0 || header[0xB2] != 0x96 || !printable(&header[0xAC], 6)) return tags;
    std::string code((const char*)&header[0xAC], 4);
    std::string maker((const char*)&header[0xB0], 2);

    // Add the publisher, the region, and the extra hardware that the code's first letter stands for
    std::string name = lookup(makerCodes, maker);
    if (!name.empty()) tags.push_back(name);
    std::string place = region(code[3]);
    if (!place.empty()) tags.push_back(place);
    switch (code[0]) {
        case 'F': tags.push_back("Classic NES"); break;
        case 'K': tags.push_back("Tilt Sensor"); break;
        case 'P': tags.push_back("e-Reader"); break;
        case 'R': tags.push_back("Rumble"); tags.push_back("Gyro Sensor"); break;
        case 'U': tags.push_back("Solar Sensor"); tags.push_back("Clock"); break;
        case 'V': tags.push_back("Rumble"); break;
    }
    if (header[0xBC]) tags.push_back("Rev " + std::to_string(header[0xBC]));
    tags.push_back(code);
    return tags;
}

std::vector<std::string> RomTags::gb(const uint8_t *header, size_t length) {
    // Only trust a header whose checksum matches (Pan Docs, The Cartridge Header)
    uint8_t check = 0;
    for (int i = 0x134; length >= 0x150 && i <= 0x14C; i++)
        check = check - header[i] - 1;
    if (length < 0x150 || check != header[0x14D]) return { "Game Boy" };

    // Name the system, with Color-only games apart from the ones that also run on the original
    std::vector<std::string> tags;
    uint8_t cgb = header[0x143];
    if (cgb == 0xC0) {
        tags.push_back("Game Boy Color");
    }
    else {
        tags.push_back("Game Boy");
        if (cgb == 0x80) tags.push_back("GBC Enhanced");
    }

    // Add the publisher from the new code if the old one says so, then the destination
    uint8_t old = header[0x14B];
    std::string name = (old == 0x33) ? lookup(gbNewLicensees, std::string((const char*)&header[0x144], 2)) :
        lookup(gbOldLicensees, old);
    if (!name.empty()) tags.push_back(name);
    tags.push_back(header[0x14A] ? "Overseas" : "Japan");

    // Add the Super Game Boy and the cartridge's extra hardware
    if (header[0x146] == 0x03 && old == 0x33) tags.push_back("Super Game Boy");
    switch (header[0x147]) {
        case 0x0F: case 0x10: case 0xFE: tags.push_back("Clock"); break;
        case 0x1C: case 0x1D: case 0x1E: tags.push_back("Rumble"); break;
        case 0x22: tags.push_back("Rumble"); tags.push_back("Tilt Sensor"); break;
        case 0xFC: tags.push_back("Camera"); break;
    }
    if (header[0x14C]) tags.push_back("Rev " + std::to_string(header[0x14C]));
    return tags;
}
