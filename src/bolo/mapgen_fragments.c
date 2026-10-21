/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapgen_fragments.c
 * Purpose:
 *   Small features cut from maps by the classic Bolo
 *   map makers (winbolo.net map collection 177). Each
 *   one is a single pod, house or walled base area, with
 *   the maker and map it came from. Made by a script;
 *   do not edit by hand.
 *
 *   B building  h halfbuilding  = road  f forest  g grass
 *   r rubble  c crater  s swamp  w river  ~ deep sea
 *   b boat  P pillbox on grass  A base on grass
 *   . not part of the fragment (left as it is)
 *********************************************************/

#include "mapgen_maker.h"

static const char *const kRows0[] = {
    ".=gggggg=.",
    "=gffffffg=",
    "gffffffffg",
    "gffBBBBffg",
    "gffBhhBffg",
    "gffBhhBffg",
    "gffBBBBffg",
    "gffffffffg",
    "=gffffffg=",
    ".=gggggg=.",
};
static const char *const kRows1[] = {
    "..===..",
    ".==B==.",
    "==BBB==",
    "=BBBBB=",
    "==BBB==",
    ".==B==.",
    "..===..",
};
static const char *const kRows2[] = {
    ".gggggggg.",
    "ggffffffgg",
    "gffffffffg",
    "gfffhhfffg",
    "gfffhhfffg",
    "gffffffffg",
    "ggffffffgg",
    ".gggggggg.",
};
static const char *const kRows3[] = {
    ".==gggg==.",
    "=PhhhhhhP=",
    "=hhhhhhhh=",
    "=hhhhhhhh=",
    "=hhhhhhhh=",
    "=hhhhhhhh=",
    "=PhhhhhhP=",
    ".==gggg==.",
};
static const char *const kRows4[] = {
    "..=gggggg=..",
    ".=gffffffg=.",
    "=gPfhhhhffg=",
    "gffhBBBBhffg",
    "gfhBBBBBBhfg",
    "gfhBBBBBBhfg",
    "gfhBBBBBBhfg",
    "gfhBBBBBBhfg",
    "gffhBBBBhffg",
    "=gffhhhhffg=",
    ".=gffffffg=.",
    "..=gggggg=..",
};
static const char *const kRows5[] = {
    "..=gggggg=..",
    ".=gffffffg=.",
    "=gffhhhhfPg=",
    "gffhBBBBhffg",
    "gfhBBBBBBhfg",
    "gfhBBBBBBhfg",
    "gfhBBBBBBhfg",
    "gfhBBBBBBhfg",
    "gffhBBBBhffg",
    "=gffhhhhffg=",
    ".=gffffffg=.",
    "..=gggggg=..",
};
static const char *const kRows6[] = {
    "..=gggggg=..",
    ".=gffffffg=.",
    "=gffhhhhffg=",
    "gffhBBBBhffg",
    "gfhBBBBBBhfg",
    "gfhBBBBBBhfg",
    "gfhBBBBBBhfg",
    "gfhBBBBBBhfg",
    "gffhBBBBhffg",
    "=gffhhhhffg=",
    ".=gffffffgA.",
    "..=gggggg=..",
};
static const char *const kRows7[] = {
    "..=gggggg=..",
    ".=gffffffg=.",
    "=gffhhhhffg=",
    "gffhBBBBhffg",
    "gfhBBBBBBhfg",
    "gfhBBBBBBhfg",
    "gfhBBBBBBhfg",
    "gfhBBBBBBhfg",
    "gffhBBBBhffg",
    "=gffhhhhffg=",
    ".Agffffffg=.",
    "..=gggggg=..",
};
static const char *const kRows8[] = {
    "...===...",
    "..==h==..",
    ".==hBh==.",
    "==hBBBh==",
    "=hBBBBBh=",
    "==hBBBh==",
    ".==hBh==.",
    "..==h==..",
    "...===...",
};
static const char *const kRows9[] = {
    "ggggggggggg",
    "gwwwwwwwwwg",
    "gwwwwwwwwwg",
    "gwwwwwwwwwg",
    "ggggggggggg",
};
static const char *const kRows10[] = {
    ".ggggggg.",
    "ggwwwwwgg",
    "gwwwwwwwg",
    "gwwwfwwwg",
    "gwwfffwwg",
    "gwwwfwwwg",
    "gwwwwwwwg",
    "ggwwwwwgg",
    ".ggggggg.",
};
static const char *const kRows11[] = {
    ".ggggg.",
    "ggwwwgg",
    "gwwwwwg",
    "gwwwwwg",
    "gwwwwwg",
    "ggwwwgg",
    ".ggggg.",
};
static const char *const kRows12[] = {
    "...ggggg...",
    "..ggwwwgg..",
    "..gwwwwwg..",
    "..gwwwwwg..",
    "gggwwwwwggg",
    "=BBgwwwgBB=",
    "==BBgggBB==",
    ".==BBgBB==.",
    "..==BBB==..",
    "...=====...",
};
static const char *const kRows13[] = {
    "...=====...",
    "..==BBB==..",
    ".==BBgBB==.",
    "==BBgggBB==",
    "=BBgwwwgBB=",
    "gggwwwwwggg",
    "..gwwwwwg..",
    "..gwwwwwg..",
    "..ggwwwgg..",
    "...ggggg...",
};
static const char *const kRows14[] = {
    ".ggggggg.",
    "ggBBBBBgg",
    "gBBBBBBBg",
    "gBBAPABBg",
    "gggg.gggg",
};
static const char *const kRows15[] = {
    "gggggggg",
    "gBgBBgBg",
    "gBBBBBBg",
    "gBgBBgBg",
    "gggggggg",
};
static const char *const kRows16[] = {
    "..====",
    "..=BBg",
    "==ABBg",
    "=BBBBg",
    "=BBBBg",
    "=ggfgf",
};
static const char *const kRows17[] = {
    "ggggg",
    "gBBBg",
    "gBBAg",
    "gBBBg",
    "ggggg",
};
static const char *const kRows18[] = {
    "ggggg",
    "gBBBg",
    "gBBBg",
    "gBBBg",
    "ggggg",
};
static const char *const kRows19[] = {
    ".ggg.",
    "ggBgg",
    "gBBBg",
    "ggBgg",
    ".ggg.",
};
static const char *const kRows20[] = {
    "========",
    "=BBBBBBg",
    "=BgggBBg",
    "=Bg.gBBg",
    "=Bgggggg",
    "=BBBg...",
    "=BBBg...",
    "=gggg...",
};
static const char *const kRows21[] = {
    "========",
    "gBBBBBB=",
    "gBBgAPB=",
    "gggg.AB=",
    "....ggB=",
    "....gBB=",
    "....gBB=",
    "....ggg=",
};
static const char *const kRows22[] = {
    "ggggg",
    "gBBBg",
    "ggBgg",
    "gBBBg",
    "ggBgg",
    "gBBBg",
    "ggggg",
};
static const char *const kRows23[] = {
    "gggggg",
    "gBAABg",
    "gBBBBg",
    "gBBBBg",
    "gggggg",
};
static const char *const kRows24[] = {
    "ggggg",
    "gBBBg",
    "ggBgg",
    "gBBBg",
    "ggggg",
};
static const char *const kRows25[] = {
    ".gggggg.",
    ".gBBBBg.",
    "ggBBBBgg",
    "gBBBBBBg",
    "gBBAABBg",
    "========",
};
static const char *const kRows26[] = {
    "gggggg",
    "gBBBBg",
    "gBBBBg",
    "gBAABg",
    "gf==fg",
};
static const char *const kRows27[] = {
    "..gggg..",
    "..fBBf..",
    "gffBBffg",
    "gBBBBBBg",
    "gBBBBBBg",
    "gffBBffg",
    "..fBBf..",
    "..gggg..",
};
static const char *const kRows28[] = {
    "gggg.",
    "=BBg.",
    "=BBgg",
    "==BBg",
    ".=BBf",
    ".=fgf",
};
static const char *const kRows29[] = {
    "ffgggg",
    "fBBBBg",
    "fBBggf",
    "fBBBg.",
    "ffBggg",
    "fBBBBg",
    "fffffg",
};
static const char *const kRows30[] = {
    "fBBf",
    "BgPB",
    "BAgB",
    "fBBf",
};
static const char *const kRows31[] = {
    "BBBBBBBBB",
    "BwwwPwwwB",
    "BwwwwwwwB",
    "BwwwwwwwB",
    "BAwwwwwAB",
    "BwwwwwwwB",
    "BwwwwwwwB",
    "BwwwPwwwB",
    "BBBBBBBBB",
};
static const char *const kRows32[] = {
    "gBBs",
    "BAsB",
    "BfPB",
    "fBBg",
};
static const char *const kRows33[] = {
    "..gBBBg..",
    ".gBfffBg.",
    "gBfffffBg",
    "BffA==ffB",
    "Bff===ffB",
    "Bff==PffB",
    "sBfffffBg",
    ".sBfffBg.",
    "..sBBBg..",
};
static const char *const kRows34[] = {
    ".gBg.",
    "gBPBg",
    "BgAgB",
    "PBgBg",
    ".gBg.",
};
static const char *const kRows35[] = {
    ".gBg.",
    "gBgBg",
    "BPAgB",
    "gBgBg",
    ".gBg.",
};
static const char *const kRows36[] = {
    "BBB",
    "BAB",
    "BgB",
    "BgB",
    "BgB",
    "BAB",
    "BBB",
};
static const char *const kRows37[] = {
    "BBBBBBB",
    "BAgggAB",
    "BBBBBBB",
};
static const char *const kRows38[] = {
    "..hhh..",
    ".hwwwh.",
    "hwwwwwh",
    "hwwwwwh",
    "hwwwwwh",
    ".hwwwh.",
    "..hhh..",
};
static const char *const kRows39[] = {
    "fgfgfgf",
    "gAggggg",
    "fgggggf",
    "ggggggg",
    "fgggggf",
    "gggPggg",
    "fgggggf",
    "ggggggg",
    "fgggggf",
    "ggggggg",
    "fgfgfgf",
};
static const char *const kRows40[] = {
    "..hh..",
    ".hwwh.",
    "hwwwwh",
    "hwwwwh",
    ".hwwh.",
    "..hh..",
};
static const char *const kRows41[] = {
    "hhhhhhh",
    "hwwwwwh",
    "hwAwPwh",
    "hwwwwwh",
    "hwPwAwh",
    "hwwwwwh",
    "hhhhhhh",
};
static const char *const kRows42[] = {
    "fgfgfgfgfgf",
    ".BBBBBBBBB.",
    "..fgfgfgf..",
    "...ggPgg...",
    "....ggg....",
};
static const char *const kRows43[] = {
    "BBBPgAgPBBB",
    "BffgggggffB",
    "BffgfffgffB",
    "ggggfffgggg",
    "gfffffffffg",
    "gfffffffffg",
    "ggggfffgggg",
    "BffgfffgffB",
    "BffgggggffB",
    "BBBgggggBBB",
};
static const char *const kRows44[] = {
    "fffffffffff",
    "fgggggggggf",
    "fgggggggggf",
    "fggPggggggf",
    "fgggggggggf",
    "fggggAggggf",
    "fgggggggggf",
    "fggggggPggf",
    "fgggggggggf",
    "fgggggggggf",
    "fffffffffff",
};
static const char *const kRows45[] = {
    "BBfffffBB",
    "BwwgggggB",
    "fwwgggggf",
    "fgggggggf",
    "fgggPgggf",
    "fgggggggf",
    "fgggggwwf",
    "BgggggwwB",
    "BBfffffBB",
};
static const char *const kRows46[] = {
    "..fffff..",
    ".fgggggf.",
    "fgPgggPgf",
    "fgggggggf",
    "fgggAgggf",
    "fgggggggf",
    "fgPgggPgf",
    ".fgggggf.",
    "..fffff..",
};
static const char *const kRows47[] = {
    "fffff",
    "fgggf",
    "fgggf",
    "fgPgf",
    "fgggf",
    ".fgf.",
    ".fgf.",
    "..f..",
    "..f..",
};
static const char *const kRows48[] = {
    "ggggggggg",
    "gfffffffg",
    "gfffffffg",
    "gfffffffg",
    "ggggggggg",
    "gBBBBBBBg",
};
static const char *const kRows49[] = {
    "ffffffff",
    "fggggggf",
    "fggggggf",
    "fggAgggf",
    "fgggPggf",
    "fggggggf",
    "fggggggf",
    "ffffffff",
};

static const MakerFragment kFragments[] = {
    { "Berserkir", "Cubosh", MAKER_FRAG_SET_BERSERKIR_POD, 10, 10, kRows0 },
    { "Berserkir", "Fitzhu", MAKER_FRAG_SET_BERSERKIR_POD, 7, 7, kRows1 },
    { "Berserkir", "Genjji", MAKER_FRAG_SET_BERSERKIR_POD, 10, 8, kRows2 },
    { "Berserkir", "Genjji", MAKER_FRAG_SET_BERSERKIR_POD, 10, 8, kRows3 },
    { "Berserkir", "Jannark", MAKER_FRAG_SET_BERSERKIR_POD, 12, 12, kRows4 },
    { "Berserkir", "Jannark", MAKER_FRAG_SET_BERSERKIR_POD, 12, 12, kRows5 },
    { "Berserkir", "Skathe", MAKER_FRAG_SET_BERSERKIR_POD, 12, 12, kRows6 },
    { "Berserkir", "Skathe", MAKER_FRAG_SET_BERSERKIR_POD, 12, 12, kRows7 },
    { "Berserkir", "Tenejam", MAKER_FRAG_SET_BERSERKIR_POD, 9, 9, kRows8 },
    { "Sluggo", "Diamond Slugdeath", MAKER_FRAG_SET_SLUGGO_CENTRE, 11, 5, kRows9 },
    { "Sluggo", "Horizontal Slugdeath", MAKER_FRAG_SET_SLUGGO_CENTRE, 9, 9, kRows10 },
    { "Sluggo", "Slugdeath Au Gratin", MAKER_FRAG_SET_SLUGGO_CENTRE, 7, 7, kRows11 },
    { "Sluggo", "Vertical Slugdeath", MAKER_FRAG_SET_SLUGGO_CENTRE, 11, 10, kRows12 },
    { "Sluggo", "Vertical Slugdeath", MAKER_FRAG_SET_SLUGGO_CENTRE, 11, 10, kRows13 },
    { "Sluggo", "Boogie Wonderland", MAKER_FRAG_SET_SLUGGO_HOUSE, 9, 5, kRows14 },
    { "Sluggo", "Boogie Wonderland", MAKER_FRAG_SET_SLUGGO_HOUSE, 8, 5, kRows15 },
    { "Sluggo", "Brockman 039 s Folly", MAKER_FRAG_SET_SLUGGO_HOUSE, 6, 6, kRows16 },
    { "Sluggo", "Brockman 039 s Folly", MAKER_FRAG_SET_SLUGGO_HOUSE, 5, 5, kRows17 },
    { "Sluggo", "Burden of Proof", MAKER_FRAG_SET_SLUGGO_HOUSE, 5, 5, kRows18 },
    { "Sluggo", "Burden of Proof", MAKER_FRAG_SET_SLUGGO_HOUSE, 5, 5, kRows19 },
    { "Sluggo", "Cavalcade of Whimsy", MAKER_FRAG_SET_SLUGGO_HOUSE, 8, 8, kRows20 },
    { "Sluggo", "Cavalcade of Whimsy", MAKER_FRAG_SET_SLUGGO_HOUSE, 8, 8, kRows21 },
    { "Sluggo", "Conjunction Junction", MAKER_FRAG_SET_SLUGGO_HOUSE, 5, 7, kRows22 },
    { "Sluggo", "Conjunction Junction", MAKER_FRAG_SET_SLUGGO_HOUSE, 6, 5, kRows23 },
    { "Sluggo", "Copacabana", MAKER_FRAG_SET_SLUGGO_HOUSE, 5, 5, kRows24 },
    { "Sluggo", "Copacabana", MAKER_FRAG_SET_SLUGGO_HOUSE, 8, 6, kRows25 },
    { "Sluggo", "Crazy Otto 039 s Diner", MAKER_FRAG_SET_SLUGGO_HOUSE, 6, 5, kRows26 },
    { "Sluggo", "Crazy Otto 039 s Diner", MAKER_FRAG_SET_SLUGGO_HOUSE, 8, 8, kRows27 },
    { "Sluggo", "Cryptical Envelopment", MAKER_FRAG_SET_SLUGGO_HOUSE, 5, 6, kRows28 },
    { "Sluggo", "Cryptical Envelopment", MAKER_FRAG_SET_SLUGGO_HOUSE, 6, 7, kRows29 },
    { "Black Lightning", "Bitter Ends", MAKER_FRAG_SET_STRONGHOLD, 4, 4, kRows30 },
    { "Black Lightning", "Even More Even Steven", MAKER_FRAG_SET_STRONGHOLD, 9, 9, kRows31 },
    { "Black Lightning", "Eye of the Needle", MAKER_FRAG_SET_STRONGHOLD, 4, 4, kRows32 },
    { "Black Lightning", "Grudge Match", MAKER_FRAG_SET_STRONGHOLD, 9, 9, kRows33 },
    { "Megawatt", "BattleZone Revisited", MAKER_FRAG_SET_STRONGHOLD, 5, 5, kRows34 },
    { "Megawatt", "BattleZone", MAKER_FRAG_SET_STRONGHOLD, 5, 5, kRows35 },
    { "Megawatt", "Four Kings", MAKER_FRAG_SET_STRONGHOLD, 3, 7, kRows36 },
    { "Megawatt", "Four Kings", MAKER_FRAG_SET_STRONGHOLD, 7, 3, kRows37 },
    { "Sluggo", "Slugfest VIII", MAKER_FRAG_SET_SLUGFEST_PANEL, 7, 7, kRows38 },
    { "Sluggo", "Slugfest XI", MAKER_FRAG_SET_SLUGFEST_PANEL, 7, 11, kRows39 },
    { "Sluggo", "Slugfest XL", MAKER_FRAG_SET_SLUGFEST_PANEL, 6, 6, kRows40 },
    { "Sluggo", "Slugfest XLI", MAKER_FRAG_SET_SLUGFEST_PANEL, 7, 7, kRows41 },
    { "Sluggo", "Slugfest XLIII", MAKER_FRAG_SET_SLUGFEST_PANEL, 11, 5, kRows42 },
    { "Sluggo", "Slugfest XVII", MAKER_FRAG_SET_SLUGFEST_PANEL, 11, 10, kRows43 },
    { "Sluggo", "Slugfest XXIII", MAKER_FRAG_SET_SLUGFEST_PANEL, 11, 11, kRows44 },
    { "Sluggo", "Slugfest XXIX", MAKER_FRAG_SET_SLUGFEST_PANEL, 9, 9, kRows45 },
    { "Sluggo", "Slugfest XXVII", MAKER_FRAG_SET_SLUGFEST_PANEL, 9, 9, kRows46 },
    { "Sluggo", "Slugfest XXVIII", MAKER_FRAG_SET_SLUGFEST_PANEL, 5, 9, kRows47 },
    { "Sluggo", "Slugfest XXX", MAKER_FRAG_SET_SLUGFEST_PANEL, 9, 6, kRows48 },
    { "Sluggo", "Slugfest XXXIV", MAKER_FRAG_SET_SLUGFEST_PANEL, 8, 8, kRows49 },
};

int makerFragmentCount(void) {
    return (int)(sizeof(kFragments) / sizeof(kFragments[0]));
}

const MakerFragment *makerFragment(int index) {
    if (index < 0 || index >= makerFragmentCount()) return 0;
    return &kFragments[index];
}
