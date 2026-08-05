/* Verification for content_writeback.c. That file is the one part of this frontend that writes over a
   file of the user. This frontend made each other file that it changes (settings.cfg and the .sav
   slots).

   ir_link_selftest.c is not part of CTest, but this test IS part of CTest. It uses usual file
   operations, and it has no IPC timing that can make the result unreliable. The properties that this
   test protects are expensive to test manually: write each kind of file back in its own shape, always
   keep the unchanged original, never leave a truncated file, and never write the file again in a loop
   over bytes that the file cannot store.

   This test needs no BIOS and no real app. A card is a raw 128KB flash image. The .mcs and .pss cases
   need only a Title Sector header that flash_load_app accepts, which is a size and a magic number. */

/* Its checks are its whole purpose, so they have to survive NDEBUG. See tests/cpu_test.c. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "content_writeback.h"

#define APP_BODY_OFFSET 8192u
#define TEST_APP_SIZE 8192u
#define TEST_MCS_SIZE (CONTENT_WRITEBACK_MCS_FRAME_SIZE + TEST_APP_SIZE)

static int failures;

#define CHECK(cond, msg)                                                                                     \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            printf("FAIL: %s\n", (msg));                                                                     \
            failures++;                                                                                      \
        }                                                                                                    \
    } while (0)

static void write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, size, f) != size) {
        printf("FAIL: could not write %s\n", path);
        failures++;
    }
    if (f) {
        fclose(f);
    }
}

/* Reads `path` into `buf`, returning its size, or (size_t)-1 if it does not exist. */
static size_t read_file(const char *path, uint8_t *buf, size_t cap) {
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return (size_t)-1;
    }
    n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static void make_card(uint8_t *card, uint8_t fill) {
    memset(card, fill, PSEMU_FLASH_SIZE);
    memcpy(card, "MC", 2); /* a real card's header magic, so this looks like what it claims to be */
}

/* A Title Sector body flash_load_app will accept: the magic at 0x52 is all it checks beyond size. */
static void make_app_body(uint8_t *body, size_t size, uint8_t fill) {
    memset(body, fill, size);
    memcpy(&body[0x52], "MCX0", 4);
}

/* A single-save export: a PS1 directory frame recording the body's size, then the body. */
static void make_mcs(uint8_t *mcs, uint8_t fill) {
    memset(mcs, 0, CONTENT_WRITEBACK_MCS_FRAME_SIZE);
    mcs[0x00] = 0x51u; /* in use, first block of the file */
    mcs[0x04] = (uint8_t)(TEST_APP_SIZE & 0xFFu);
    mcs[0x05] = (uint8_t)((TEST_APP_SIZE >> 8) & 0xFFu);
    memcpy(&mcs[0x0A], "BASLUS-99999-TEST", 17);
    make_app_body(mcs + CONTENT_WRITEBACK_MCS_FRAME_SIZE, TEST_APP_SIZE, fill);
}

/* An app changes flash only during execution. This test does not execute an app, because it tests the
   file operations and not the emulator. A replacement of all of flash is the same change for the
   write-back code, and it needs no BIOS. */
static void poke_flash(psemu_t *ps, uint32_t offset, uint8_t value) {
    static uint8_t buf[PSEMU_FLASH_SIZE];
    psemu_save_flash_image(ps, buf, sizeof(buf));
    buf[offset] = value;
    psemu_load_flash_image(ps, buf, sizeof(buf));
}

static void settle(content_writeback_t *cw, psemu_t *ps) {
    unsigned long frame;
    for (frame = 0; frame <= CONTENT_WRITEBACK_SETTLE_FRAMES; frame++) {
        content_writeback_poll(cw, ps, frame);
    }
}

static void remove_all(const char *path) {
    char side[MAX_PATH + 64];
    DeleteFileA(path);
    snprintf(side, sizeof(side), "%s.bak", path);
    DeleteFileA(side);
    snprintf(side, sizeof(side), "%s.tmp", path);
    DeleteFileA(side);
}

/* ---- whole-card .mcr ---------------------------------------------------------------------------- */

static void test_card(const char *dir) {
    static uint8_t card[PSEMU_FLASH_SIZE];
    static uint8_t got[PSEMU_FLASH_SIZE];
    static content_writeback_t cw;
    char path[MAX_PATH + 32];
    char bak_path[MAX_PATH + 64];
    char tmp_path[MAX_PATH + 64];
    psemu_t *ps;
    unsigned long frame;
    size_t n;

    snprintf(path, sizeof(path), "%scontent_writeback_selftest.mcr", dir);
    snprintf(bak_path, sizeof(bak_path), "%s.bak", path);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    remove_all(path);

    make_card(card, 0x11);
    write_file(path, card, PSEMU_FLASH_SIZE);

    ps = psemu_create();
    psemu_load_flash_image(ps, card, PSEMU_FLASH_SIZE);
    content_writeback_arm(&cw, ps, path, card, PSEMU_FLASH_SIZE);
    CHECK(cw.enabled && cw.kind == PSEMU_CONTENT_CARD, "a whole-card load should arm as a card");

    /* This code never writes an unchanged card again. Thus an app that changes nothing does not
       change the file of the user. */
    for (frame = 0; frame < CONTENT_WRITEBACK_SETTLE_FRAMES * 2; frame++) {
        content_writeback_poll(&cw, ps, frame);
    }
    CHECK(!cw.dirty, "an unchanged card should never go dirty");
    CHECK(read_file(bak_path, got, sizeof(got)) == (size_t)-1, "an unchanged card should not produce a .bak");

    /* The PS1-save case: the app edits the game's save in block 1 (see docs/app-notes.md). */
    poke_flash(ps, 0x2000u + 0x259u, 0x01);
    for (frame = 0; frame < CONTENT_WRITEBACK_SETTLE_FRAMES; frame++) {
        content_writeback_poll(&cw, ps, frame);
    }
    CHECK(cw.dirty, "an edited card should be dirty before the settle window elapses");
    n = read_file(path, got, sizeof(got));
    CHECK(n == PSEMU_FLASH_SIZE && got[0x2000u + 0x259u] == 0x11,
        "nothing should be committed before the settle window elapses");

    content_writeback_poll(&cw, ps, CONTENT_WRITEBACK_SETTLE_FRAMES);
    CHECK(!cw.dirty, "committing should clear dirty");
    n = read_file(path, got, sizeof(got));
    CHECK(n == PSEMU_FLASH_SIZE, "the committed card should still be exactly one card");
    CHECK(n == PSEMU_FLASH_SIZE && got[0x2000u + 0x259u] == 0x01, "the edit should have reached the file");

    /* An app that saves its OWN state uses the same path. This test is important because this
       function was made for the PS1-save condition. An app reaches its own blocks through the FLASH1
       banked window, but flash1_write8 resolves the bank into the same storage
       (core/src/flash.c). */
    poke_flash(ps, 0x4000u + 0x100u, 0x42);
    settle(&cw, ps);
    n = read_file(path, got, sizeof(got));
    CHECK(n == PSEMU_FLASH_SIZE && got[0x4000u + 0x100u] == 0x42,
        "a write to the app's own block should reach the file too");

    /* The pristine original, kept exactly once. */
    n = read_file(bak_path, got, sizeof(got));
    CHECK(n == PSEMU_FLASH_SIZE, "a backup of the original should exist");
    CHECK(n == PSEMU_FLASH_SIZE && got[0x2000u + 0x259u] == 0x11 && got[0x4000u + 0x100u] == 0x11,
        "the backup should hold the ORIGINAL bytes, not any commit");
    CHECK(read_file(tmp_path, got, sizeof(got)) == (size_t)-1, "no .tmp should survive a successful commit");

    /* An edit that is undone before the window elapses leaves the file alone. */
    poke_flash(ps, 0x2000u + 0x25Bu, 0x03);
    content_writeback_poll(&cw, ps, 0);
    CHECK(cw.dirty, "the transient edit should register as dirty");
    poke_flash(ps, 0x2000u + 0x25Bu, 0x11);
    content_writeback_poll(&cw, ps, 1);
    CHECK(!cw.dirty, "an edit that is undone should clear dirty without committing");

    /* resync adopts a wholesale replacement (a reset, a save-state load) rather than committing it. */
    poke_flash(ps, 0x2000u + 0x25Cu, 0x04);
    content_writeback_poll(&cw, ps, 0);
    CHECK(cw.dirty, "a wholesale replacement still shows up as a change");
    content_writeback_resync(&cw, ps);
    CHECK(!cw.dirty, "resync should clear dirty");
    CHECK(!content_writeback_commit(&cw, ps), "resync should leave nothing to commit");
    n = read_file(path, got, sizeof(got));
    CHECK(n == PSEMU_FLASH_SIZE && got[0x2000u + 0x25Cu] != 0x04, "resync should not have written the file");

    psemu_destroy(ps);
    remove_all(path);
    printf("  card: done\n");
}

/* ---- single-save .mcs --------------------------------------------------------------------------- */

static void test_mcs(const char *dir) {
    static uint8_t mcs[TEST_MCS_SIZE];
    static uint8_t got[PSEMU_FLASH_SIZE];
    static content_writeback_t cw;
    char path[MAX_PATH + 32];
    char bak_path[MAX_PATH + 64];
    psemu_t *ps;
    size_t n;

    snprintf(path, sizeof(path), "%scontent_writeback_selftest.mcs", dir);
    snprintf(bak_path, sizeof(bak_path), "%s.bak", path);
    remove_all(path);

    make_mcs(mcs, 0x33);
    write_file(path, mcs, sizeof(mcs));

    ps = psemu_create();
    CHECK(psemu_load_content(ps, mcs, sizeof(mcs)) == PSEMU_OK, "the test .mcs should load");
    content_writeback_arm(&cw, ps, path, mcs, sizeof(mcs));
    CHECK(cw.enabled && cw.kind == PSEMU_CONTENT_MCS, "a .mcs load should arm as a .mcs");
    CHECK(cw.region_size == TEST_APP_SIZE, "the writable region should be the app body alone");

    /* The app saves its own progress. For a .mcs that is the only kind of write there is. */
    poke_flash(ps, APP_BODY_OFFSET + 0x400u, 0x77);
    settle(&cw, ps);
    n = read_file(path, got, sizeof(got));
    CHECK(n == TEST_MCS_SIZE, "a rebuilt .mcs must be exactly as long as the one that was loaded");
    CHECK(n == TEST_MCS_SIZE && got[CONTENT_WRITEBACK_MCS_FRAME_SIZE + 0x400u] == 0x77,
        "the app's save should reach the .mcs body");
    CHECK(n == TEST_MCS_SIZE && memcmp(got, mcs, CONTENT_WRITEBACK_MCS_FRAME_SIZE) == 0,
        "the directory frame should come back verbatim");
    CHECK(n == TEST_MCS_SIZE && memcmp(&got[0x0A], "BASLUS-99999-TEST", 17) == 0,
        "the file name in the frame should survive a rebuild");

    /* The rebuilt file has to load again, or the round trip is not one. */
    CHECK(psemu_identify_content(got, n) == PSEMU_CONTENT_MCS, "a rebuilt .mcs should still identify as one");
    CHECK(psemu_load_content(ps, got, n) == PSEMU_OK, "a rebuilt .mcs should load again");

    n = read_file(bak_path, got, sizeof(got));
    CHECK(n == TEST_MCS_SIZE, "a .mcs should get a backup too");
    CHECK(n == TEST_MCS_SIZE && got[CONTENT_WRITEBACK_MCS_FRAME_SIZE + 0x400u] == 0x33,
        "the .mcs backup should hold the original body");

    /* The synthesized card around the app is not in the file, and it cannot be in the file. A write to
       that card must not mark the file as changed. If it does, each frame writes the same bytes
       again, permanently. */
    psemu_load_content(ps, mcs, sizeof(mcs));
    content_writeback_arm(&cw, ps, path, mcs, sizeof(mcs));
    poke_flash(ps, 0x40u, 0x5A);          /* the synthesized directory, block 0 */
    poke_flash(ps, 0x2000u * 3u, 0x5A);   /* a block no loaded app owns */
    settle(&cw, ps);
    CHECK(!cw.dirty, "a write outside the app's own body must not mark a .mcs dirty");

    psemu_destroy(ps);
    remove_all(path);
    printf("  mcs: done\n");
}

/* ---- bare .pss ---------------------------------------------------------------------------------- */

static void test_pss(const char *dir) {
    static uint8_t body[TEST_APP_SIZE];
    static uint8_t got[PSEMU_FLASH_SIZE];
    static content_writeback_t cw;
    char path[MAX_PATH + 32];
    psemu_t *ps;
    size_t n;

    snprintf(path, sizeof(path), "%scontent_writeback_selftest.pss", dir);
    remove_all(path);

    make_app_body(body, sizeof(body), 0x55);
    write_file(path, body, sizeof(body));

    ps = psemu_create();
    CHECK(psemu_load_content(ps, body, sizeof(body)) == PSEMU_OK, "the test .pss should load");
    content_writeback_arm(&cw, ps, path, body, sizeof(body));
    CHECK(cw.enabled && cw.kind == PSEMU_CONTENT_APP, "a .pss load should arm as a bare app");

    poke_flash(ps, APP_BODY_OFFSET + 0x800u, 0x66);
    settle(&cw, ps);
    n = read_file(path, got, sizeof(got));
    CHECK(n == TEST_APP_SIZE, "a rebuilt .pss must be exactly as long as the one that was loaded");
    CHECK(n == TEST_APP_SIZE && got[0x800u] == 0x66, "the app's save should reach the .pss");
    CHECK(psemu_load_content(ps, got, n) == PSEMU_OK, "a rebuilt .pss should load again");

    psemu_destroy(ps);
    remove_all(path);
    printf("  pss: done\n");
}

/* ---- content this frontend must not write ------------------------------------------------------- */

static void test_unrecognised(const char *dir) {
    static uint8_t junk[4096];
    static uint8_t got[PSEMU_FLASH_SIZE];
    static content_writeback_t cw;
    char path[MAX_PATH + 32];
    psemu_t *ps;

    snprintf(path, sizeof(path), "%scontent_writeback_selftest.bin", dir);
    remove_all(path);

    memset(junk, 0x99, sizeof(junk)); /* no Title Sector magic, no directory frame */
    write_file(path, junk, sizeof(junk));

    ps = psemu_create();
    content_writeback_arm(&cw, ps, path, junk, sizeof(junk));
    CHECK(!cw.enabled, "unrecognised content must not arm write-back");
    poke_flash(ps, APP_BODY_OFFSET, 0x01);
    settle(&cw, ps);
    CHECK(!content_writeback_commit(&cw, ps), "a disarmed write-back must never write");
    CHECK(read_file(path, got, sizeof(got)) == sizeof(junk), "the file should be untouched");

    /* And the no-content case a frontend hits before anything is opened. */
    content_writeback_arm(&cw, ps, path, NULL, 0);
    CHECK(!cw.enabled, "a NULL load must not arm write-back");

    psemu_destroy(ps);
    remove_all(path);
    printf("  unrecognised: done\n");
}

/* An optional mode. CTest does not use it. It round-trips a REAL file that the caller names. The tests
   above build their own content, thus they can execute in each environment. This includes CI, where
   the testdata/ directory does not exist, because .gitignore excludes it. Thus those tests cannot show
   one property: that the file of a real app comes back byte for byte when nothing changes it. Start
   this mode manually against a file in testdata/ after a change to the rebuild path:

     content_writeback_selftest testdata/<your app file>.mcs

   A load with no change must build the input again exactly. Each difference is a fault in the rebuild
   code, because no emulation executed. */
static void test_real_file(const char *path) {
    static uint8_t original[PSEMU_FLASH_SIZE];
    static uint8_t got[PSEMU_FLASH_SIZE];
    static content_writeback_t cw;
    char copy_path[MAX_PATH + 64];
    char dir[MAX_PATH];
    psemu_t *ps;
    size_t size, n;
    const char *kind_name[] = { "unrecognised", "card (.mcr)", "single save (.mcs)", "bare app (.pss)" };

    size = read_file(path, original, sizeof(original));
    if (size == (size_t)-1) {
        printf("FAIL: could not read %s\n", path);
        failures++;
        return;
    }
    /* Against a copy, never the original: this writes to whatever it is pointed at. */
    GetTempPathA(sizeof(dir), dir);
    snprintf(copy_path, sizeof(copy_path), "%scontent_writeback_realfile.tmpcopy", dir);
    remove_all(copy_path);
    write_file(copy_path, original, size);

    ps = psemu_create();
    printf("  %s: %zu bytes, %s, identity 0x%08X\n", path, size, kind_name[psemu_identify_content(original, size)],
        psemu_content_identity_hash(original, size));
    CHECK(psemu_load_content(ps, original, size) == PSEMU_OK, "the real file should load");
    content_writeback_arm(&cw, ps, copy_path, original, size);
    CHECK(cw.enabled, "a real app/card file should arm write-back");

    /* Force a commit of content nothing has touched, so the only thing under test is the rebuild. */
    cw.dirty = 1;
    cw.dirty_since_frame = 0;
    CHECK(content_writeback_commit(&cw, ps), "the forced commit should write");
    n = read_file(copy_path, got, sizeof(got));
    CHECK(n == size, "a rebuilt real file must be the same length");
    CHECK(n == size && memcmp(got, original, size) == 0,
        "an unchanged real file must rebuild byte-for-byte identically");

    psemu_destroy(ps);
    remove_all(copy_path);
}

int main(int argc, char **argv) {
    char dir[MAX_PATH];
    GetTempPathA(sizeof(dir), dir);

    if (argc > 1) {
        int i;
        for (i = 1; i < argc; i++) {
            test_real_file(argv[i]);
        }
        if (failures) {
            printf("content_writeback_selftest: %d failure(s)\n", failures);
            return 1;
        }
        printf("content_writeback_selftest: real-file round trips passed\n");
        return 0;
    }

    test_card(dir);
    test_mcs(dir);
    test_pss(dir);
    test_unrecognised(dir);

    if (failures) {
        printf("content_writeback_selftest: %d failure(s)\n", failures);
        return 1;
    }
    printf("content_writeback_selftest: all checks passed\n");
    return 0;
}
