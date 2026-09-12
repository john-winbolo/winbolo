/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * A brainrec session file from an older build has to be refused, not read.
 *
 * brainrec.btr frames are runs of raw snapshot structs — the writer dumps
 * sizeof(TankSnapshot) and sizeof(PillSnapshot) straight to the file — so a
 * file written against different structs cannot be walked at all: the reader
 * would be out of step from the first frame, not wrong in one field. Version 5
 * is exactly that file. Its TankSnapshot carries an 8-bit death wait and its
 * PillSnapshot packs armour into the flags byte, both of which have since
 * changed width, so version 6 refuses it.
 *
 * BrainTest's loader is a GUI main with no seam a test can call, so what this
 * asserts is the check the loader now makes: brainRecMagicMatches and
 * brainRecVersionMatches in brain_record.h, which both of its header reads go
 * through (btPeekSession and btLoadSession in src/braintest/braintest_main.c).
 */
#include <string.h>

#include "global.h"
#include "brain_record.h"
#include "test_harness.h"

/* A header as it sits at the top of a file: the magic, a version, a map name.
 * Written field by field rather than memcpy'd from a struct literal so the
 * case does not quietly inherit whatever the current struct happens to be. */
static void brFillHeader(BrainRecHeader *hdr, uint32_t version) {
    memset(hdr, 0, sizeof(*hdr));
    memcpy(hdr->magic, BRAINREC_MAGIC, BRAINREC_MAGIC_LEN);
    hdr->version = version;
    memcpy(hdr->mapName, "Everard Island", 14);
}

int run_brainrec_version_rejects_old(void) {
    BrainRecHeader hdr;

    /* The version this build writes is read back. */
    brFillHeader(&hdr, BRAINREC_VERSION);
    UT_ASSERT_MSG(brainRecMagicMatches(&hdr),
                  "the writer's own magic did not match");
    UT_ASSERT_MSG(brainRecVersionMatches(&hdr),
                  "version %u is what this build writes and it was refused",
                  (unsigned) BRAINREC_VERSION);

    /* Version 5 — the last one before the snapshot structs changed size — is
     * refused. Its magic is the same, which is the point: only the version
     * byte separates a file that can be read from one that cannot. */
    brFillHeader(&hdr, 5u);
    UT_ASSERT_MSG(brainRecMagicMatches(&hdr),
                  "a version 5 file has the same magic and should match it");
    UT_ASSERT_MSG(!brainRecVersionMatches(&hdr),
                  "a version 5 brainrec was accepted by a build that reads %u",
                  (unsigned) BRAINREC_VERSION);

    /* And a version from the future is refused the same way, so the check is
     * an equality rather than a floor: a newer file's frames are no more
     * walkable than an older one's. */
    brFillHeader(&hdr, BRAINREC_VERSION + 1u);
    UT_ASSERT_MSG(!brainRecVersionMatches(&hdr),
                  "a version %u brainrec was accepted by a build that reads %u",
                  (unsigned) (BRAINREC_VERSION + 1u),
                  (unsigned) BRAINREC_VERSION);

    /* A file that is not a brainrec at all fails on the magic, whatever its
     * version field says. */
    brFillHeader(&hdr, BRAINREC_VERSION);
    hdr.magic[0] = 'X';
    UT_ASSERT_MSG(!brainRecMagicMatches(&hdr),
                  "a file with the wrong magic was taken for a brainrec");

    UT_ASSERT_MSG(BRAINREC_VERSION == 6u,
                  "BRAINREC_VERSION is %u; the widened death wait and the "
                  "pillbox's own armour byte are version 6",
                  (unsigned) BRAINREC_VERSION);
    return 0;
}
