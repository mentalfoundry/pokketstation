#include <SDL.h>
#include <SDL_syswm.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#ifdef _MSC_VER
/* SysLink is the clickable repository link in Help > About.
   SysLink exists only in ComCtl32 v6 and later versions.
   Without this manifest, the operating system loader uses the older v5.82 system DLL.
   That older DLL has no manifest, thus it permits no side-by-side version selection.
   Without v6, CreateDialog fails to make the SysLink control, and gives no error. */
#pragma comment(linker, \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "psemu/psemu.h"
#include "resource.h"
#include "content_writeback.h"
#include "ir_link.h"

#define SCALE 8

/* Help > About shows this value.
   Change this value manually for each release, to agree with the latest git tag.
   This project does not connect this value to git or to CMake automatically.
   This string is the only location to change for a new release.
   A build from a source archive has no git. Thus an automatic value can become incorrect, and give
   no error. */
#define POKKETSTATION_VERSION "v1.11.1"

/* Returns the directory of the running executable.
   This function gets the directory from argv[0]. It does not use an operating-system interface for
   the "current module path".
   When a user starts the executable with a double click, the file manager supplies the full path in
   argv[0]. That is the one condition that needs this function.
   A command-line start supplies explicit paths, and it never uses this code. */
static void get_exe_dir(const char *argv0, char *out, size_t out_size) {
    const char *last_sep = NULL;
    const char *p;
    for (p = argv0; *p; p++) {
        if (*p == '/' || *p == '\\') {
            last_sep = p;
        }
    }
    size_t len = last_sep ? (size_t)(last_sep - argv0) : 0;
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, argv0, len);
    out[len] = '\0';
}

static void join_path(char *out, size_t out_size, const char *dir, const char *name) {
    if (dir[0] == '\0') {
        snprintf(out, out_size, "%s", name);
    } else {
        snprintf(out, out_size, "%s/%s", dir, name);
    }
}

/* The number of save slots in File > Save State and File > Load State.
   Slot 0 is the quick slot. It is the only slot with a key binding (see
   button_scancodes). Only the menu can reach slots 1 and 2. Thus an incorrect
   hotkey cannot destroy a state in one of those slots, which is their purpose.
   To increase this number, you must add the applicable menu items, and make the
   ID_FILE_SAVE_SLOT_BASE and ID_FILE_LOAD_SLOT_BASE ranges in resource.h and the
   .rc file larger. No other code here has a fixed limit of 3. */
#define SAVE_SLOT_COUNT 3

/* Save State and Load State use one file for each loaded app or card, in each
   slot.
   The file name is the file name and extension of the app or card. Thus a user
   who examines the directory of the executable can quickly see which save
   belongs to which app or card. This code gets the name from app_path, and does
   not keep a copy, because the loaded app can change during a session.
   Slot 0 keeps the name "<name>.sav" that the single slot always used. Thus
   saves from before the slots existed still load. Slots 1 and 2 add "_1" or "_2"
   after the extension, for example "mycard.mcr_1.sav". Thus they sort next to
   the file of slot 0, and the extension does not separate them.
   Two different apps or cards with the same file name, for example from
   different directories, use the same save file. The app_size and app_hash test
   in the header below still refuses a file that does not agree. Thus this
   condition can only cause a lost save. It can never load the incorrect
   state. */
static void get_save_slot_path(char *out, size_t out_size, const char *exe_dir, const char *app_path, int slot) {
    const char *base = app_path;
    const char *slash;
    char name[1024];
    for (slash = app_path; *slash != '\0'; slash++) {
        if (*slash == '/' || *slash == '\\') {
            base = slash + 1;
        }
    }
    if (slot == 0) {
        snprintf(name, sizeof(name), "%s.sav", base);
    } else {
        snprintf(name, sizeof(name), "%s_%d.sav", base, slot);
    }
    join_path(out, out_size, exe_dir, name);
}

/* Gives a name to a slot, for the message boxes below. Thus a failure message
   identifies one of the three slots, and does not say only "the save state".
   The name is part of a sentence, for example "...found in the quick slot...".
   This is the reason for the lower-case letters. */
static const char *save_slot_label(int slot) {
    static const char *const labels[SAVE_SLOT_COUNT] = {"the quick slot", "slot 1", "slot 2"};
    return (slot >= 0 && slot < SAVE_SLOT_COUNT) ? labels[slot] : "an unknown slot";
}

#define QUICKSAVE_MAGIC "PKQS"
/* 2: psemu_t received the RAM write lock for psemu_set_volume_override. Thus
   the size of the raw state data changed. This code refuses a version-1 file.
   It does not read such a file as a truncated version-2 state.
   3: psemu_t received the app-execution tracking for psemu_app_running. Thus
   the size changed again. The state data is a raw dump of the structure, and
   psemu_state_size is sizeof(psemu_t). Thus each new field in psemu_t needs a
   new version number here. The size test in psemu_load_state finds only a
   state that became smaller. It never finds a state that became larger.
   4: arm7tdmi_t received r8_12_bank, the FIQ bank for r8-r12. A real ARM7TDMI
   has that bank, and this emulator did not model it. If this code reads a
   version-3 state as version 4, each field after the CPU is incorrect. Thus
   this code refuses such a state.
   5: the meaning of app_hash changed. See below. The state data did not
   change. But the hash of a version-4 file never agrees with the hash of a
   version-5 file. A message that says "this state does not belong to this
   card" is worse than a message that says "this state is from an older
   build".
   6: psemu_t received com_t, the communication port to a PS1 (see
   core/src/com.h). That field is between flash_t and ir_t. Thus the size
   changed, and the offset of each field after flash_t also changed. A build
   before this version reads a version-6 state as a version-5 state, because
   the size test finds only a state that became smaller. Each field after
   flash_t is then incorrect. This version number refuses that load.
   7: the core replaced the raw copy of psemu_t with a field-by-field format
   (see core/src/state.c). The state data is different, thus this file version
   changes one time for that.

   THIS VERSION NUMBER NO LONGER TRACKS THE LAYOUT OF psemu_t. The core owns
   the version of its own state data now, in PSEMU_STATE_VERSION, and
   psemu_load_state returns PSEMU_ERR_BAD_FORMAT for a file that it cannot
   read. Thus a new field in psemu_t no longer needs a change here. Change
   this number only for a change to the parts of the file that this app owns:
   the header, the app size, and the app hash. */
#define QUICKSAVE_VERSION 7u

/* app_size and app_hash prevent a load of a state that a different app or card
   made. The file name for each app already makes such a load improbable. This
   test is a safety measure against a hash collision, or against a file that a
   user edited or gave a new name.

   app_hash is a hash of the IDENTITY of the content. It is not a hash of the
   file (see psemu_content_identity_hash). This code hashed the full file until
   the write-back function existed. That method stopped operating correctly
   immediately: an app that saves its progress writes the file again, thus each
   state from before that save stopped agreeing with its own card. A hash of
   only the parts that identify the card, which are the directory file names
   and the title-sector metadata, continues to agree after an app saves. It
   still refuses a different app or a different card. */
typedef struct {
    char magic[4];
    uint32_t version;
    uint32_t app_size;
    uint32_t app_hash;
} quicksave_header_t;

/* Writes a diagnostic report with a timestamp to disk, and gives its name to the user on stderr.
   The report contains data from the frontend: the reason string and the frame number.
   The report also contains the full CPU dump and trace dump from psemu_write_crash_report.
   Two conditions cause this function: an automatic call at a detected CPU fault, and a call from a
   hotkey. That hotkey is F12 by default, and Tools > Remap Controls... can change it.
   Not each condition that needs a report causes psemu_cpu_faulted() to return a nonzero value. "The
   app looks incorrect" and "there is no sound" are also real conditions.
   An earlier crash investigation (see docs/hardware-notes.md) needed this exact kind of state dump,
   and that dump had to be built manually with temporary trace code. */
static void write_diagnostic_report(
    const psemu_t *ps, const char *reason, unsigned long frame, const char *bios_path, const char *app_path) {
    char path[64];
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    FILE *f;

    strftime(path, sizeof(path), "pokketstation_report_%Y%m%d_%H%M%S.log", tmv);
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "psemu: failed to write diagnostic report to %s\n", path);
        return;
    }
    fprintf(f, "reason: %s\n", reason);
    fprintf(f, "bios: %s\n", bios_path);
    fprintf(f, "app: %s\n", app_path);
    fprintf(f, "frame: %lu\n", frame);
    psemu_write_crash_report(ps, f);
    fclose(f);
    fprintf(stderr, "psemu: wrote diagnostic report to %s\n", path);
}

/* The small preferences file of this app.
   No external tool reads or writes this format.
   This format is different from the encoding of the hardware ID (see the comment
   on psemu_parse_hardware_id in psemu.h).

   settings.cfg holds these values:
     - The path of the last BIOS that loaded correctly.
       Thus a start with a double click, and no command-line arguments, operates
       without a bios.bin file next to the executable. The user must first select a
       real BIOS one time, with File > Open or with a command-line argument.
     - The hardware serial number of the PocketStation (F_SN).
       The default value of the core (PSEMU_DEFAULT_HARDWARE_ID) already gives the
       best rank to each new save of one companion app.
       A homebrew ID-editor app can change this value during a session.
       No other permanent storage for this value exists: the value is in a flash
       "header" region outside the usual 128KB card image. Thus it is not part of a
       .mcr or .mcs file.
       This app stores the value as exactly 8 hex digits. A real homebrew "ID
       rewriter" app shows and changes the same form.
       Real hardware confirms that the first digit does not have to be a letter: a
       real unit accepts and keeps the value "EEEEEEEE".
       An empty value means "use the default value".
     - Whether the app shows a console window (show_console). This app has no
       control for this value. Only a manual edit of settings.cfg changes it.
     - Whether the window title shows live IR link counters
       (ir_link_diagnostics). This value is off by default, and only a manual edit
       changes it. The counters report the edges that the link sent, received,
       discarded, and received too late for correct placement. That last counter is
       the reason for this function: a link can report "Connected", with equal sent
       and received counts and no discarded edges, and still decode nothing. No
       other symptom separates those two conditions. These counters are useful
       during the diagnosis of a link that does not transfer data, and they have no
       meaning at other times. They also change at each frame. Thus they stay off
       until a user asks for them.
       --console and --no-console change the value for one run, the same as each
       other command-line flag. They do not write to settings.cfg.
       A flag on the command line for one run is not the same as a permanent
       preference, and it must not replace one.

   This app writes settings.cfg immediately when a value changes: when a BIOS loads
   from a command-line argument or from File > Open, and when a user changes the
   hardware ID with Tools > Edit Hardware ID. This app does not collect the changes
   for one write at exit. With one write at exit, a forced stop or a crash during a
   session loses each change from that session, and gives no error. */
#define SETTINGS_CONFIG_NAME "settings.cfg"

typedef struct {
    char bios_path[1024];
    char hardware_id[PSEMU_HARDWARE_ID_STRING_SIZE];
    /* "RRGGBB" in hex, or empty for the default value of the Classic preset.
       A new settings.cfg file starts with the Classic preset.
       See load_settings and the DISPLAY_*_CLASSIC constants below. */
    char pixel_color[7];
    char bg_color[7];
    /* "RRGGBB" in hex, or empty for the default value of DISPLAY_SHADOW_COLOR. */
    char shadow_color[7];
    int show_console;
    int show_shadows;
    /* Adds live IR link edge counters to the window title while the link is connected. This value is
       off by default: the numbers have no meaning to a person who only uses the link, and they change
       at each frame. See ir_link_diagnostics in the comment on load_settings above. */
    int ir_link_diagnostics;
    /* Key names in the format of SDL_GetScancodeName(), for example "Up", "Z", or "Left Ctrl".
       An empty value means "use the default key for that button". See resolve_key_binding. */
    char key_up[32];
    char key_down[32];
    char key_left[32];
    char key_right[32];
    char key_fire[32];
    /* This key is not a PocketStation button. It causes write_diagnostic_report
       (see button_scancodes in main). */
    char key_debug_log[32];
    /* These keys are not PocketStation buttons. They cause reset_emulation, and
       save_state_to_slot and load_state_from_slot for the quick slot (see
       button_scancodes in main). */
    char key_reset[32];
    char key_quick_save[32];
    char key_quick_load[32];
    /* Tools > Date/Time Override.
       This value holds a BIOS setting that is in emulated RAM, and not in a
       hardware register (see docs/hardware-notes.md). DATETIME_OVERRIDE_OFF
       means "make no change", thus the system menus of the PocketStation operate
       normally. For each other value, this app writes the setting again at each
       frame. That repetition keeps the value against the BIOS menu, which writes
       its own value. */
    int datetime_override;
    /* Tools > Sound > Volume. The range is 0 to 100, in percent. This is the
       output level of the emulator, and the emulated machine cannot read it. It
       scales the PCM data on the path to the audio device, after the core applies
       the volume setting of the PocketStation. Thus the two values multiply.
       This structure also had a volume_override value, which held that
       PocketStation setting in the same way that datetime_override holds the
       clock. That value is now removed: it gave three coarse steps, and only on a
       traced BIOS, for a function that master_volume does better on each BIOS.
       This app ignores a volume_override= line in an older settings.cfg file, and
       removes the line at the next save. */
    int master_volume;
    /* Tools > Sound > Speaker. One of the SPEAKER_SIM_* values below. Like
       master_volume, this is only an output effect, and the emulated machine
       cannot read it. This app applies it before master_volume. Thus the
       percentage stays a simple loudness control above it. */
    int speaker_sim;
} app_settings_t;

#define DATETIME_OVERRIDE_OFF 0
#define DATETIME_OVERRIDE_OS 1

/* Tools > Sound > Volume. The menu gives steps of 10%, but this app stores the setting as a
   percentage. Thus a control with a finer resolution later, for example a slider or a
   hotkey, needs no new format on disk. Such a control gives only a value that the menu
   cannot produce, and sound_menu_id below rounds that value to the nearest step that the
   menu can show. */
#define MASTER_VOLUME_MAX 100
#define MASTER_VOLUME_DEFAULT 100

static int clamp_master_volume(int percent) {
    if (percent < 0) {
        return 0;
    }
    if (percent > MASTER_VOLUME_MAX) {
        return MASTER_VOLUME_MAX;
    }
    return percent;
}

/* The percentage is a loudness, and not an amplitude. Thus this code does not
   multiply the samples by the percentage.
   Human hearing is logarithmic: the perceived loudness becomes approximately one
   half for each 10 dB of attenuation. To multiply the samples by 0.5 gives only
   -6 dB, which sounds approximately two thirds as loud. Thus a "50%" value is
   clearly not one half. One half of the loudness needs -10 dB, which is an
   amplitude of 0.316.
   The rule "each division of the percentage by 2 costs 10 dB" gives
       amplitude = p ^ (log2(10) / 2) = p ^ 1.660964,  where p = percent / 100
   This is the exponent below. It gives 0 dB at 100%, -10 dB at 50% (one half of
   the loudness), -20 dB at 25% (one quarter), and -33 dB at 10%.
   There is sufficient range for this: dac.c scales the 10-bit DAC field to the
   full int16 range before the data gets here. Thus the -33 dB step still leaves a
   waveform of approximately 700 counts, and the waveform does not become
   noise. */
#define MASTER_VOLUME_LOUDNESS_EXPONENT 1.6609640474436813

/* Attenuates the samples of one frame in the same buffer.
   At 100%, this function returns and does not change the buffer. Thus the default
   path has no cost. */
static void apply_master_volume(int16_t *samples, uint32_t count, int percent) {
    if (percent >= MASTER_VOLUME_MAX) {
        return;
    }
    if (percent <= 0) {
        memset(samples, 0, (size_t)count * sizeof(samples[0]));
        return;
    }
    /* One pow() call for each frame, and then integer arithmetic for each sample.
       The format is Q16.16. The gain is less than 1.0 on each path that gets
       here, thus gain_q16 is 65535 or less. Thus the product of gain_q16 and a
       full-scale sample stays in the range of an int32. */
    double gain = pow((double)percent / (double)MASTER_VOLUME_MAX, MASTER_VOLUME_LOUDNESS_EXPONENT);
    int32_t gain_q16 = (int32_t)(gain * 65536.0 + 0.5);
    for (uint32_t i = 0; i < count; i++) {
        samples[i] = (int16_t)(((int32_t)samples[i] * gain_q16) / 65536);
    }
}

/* Tools > Sound > Speaker.

   The core gives the frontend exactly the data that the DAC held, sample for
   sample (see dac.c). That data is the signal at the *terminals* of the
   PocketStation speaker. It is not the sound that a person hears from the
   device. The speaker of a real unit is a transducer of approximately 1cm, in a
   plastic shell with no true enclosure. It reproduces almost nothing below its
   own resonance, which is in the 1kHz to 2kHz region, and its output decreases
   quickly below that frequency.

   A laptop speaker or a desktop speaker has a true low-frequency response. Thus
   the raw signal on such a speaker reproduces each frequency that the real
   device could not produce, and the result sounds thick and unclear against the
   hardware. The content makes this worse: software controls the DAC one held
   level at a time. Thus almost each tone from an app is near to a square wave,
   and its lowest harmonics hold most of the energy. The signal also has the DC
   offset of the last DACV level.

   Thus this code filters the frequencies that the real speaker removed
   mechanically. It uses a second-order high-pass filter (an RBJ cookbook
   biquad). That shape is correct, because a driver below its resonance decreases
   its output by approximately 12dB for each octave. Q sets the size of the
   resonant peak at the corner frequency. A small, inexpensive speaker has a
   clear peak, which is a large part of the reason for its sound.

   Each preset is approximately 4dB quieter than the raw signal. This is not a
   tuning error to correct: the low frequencies that the filter removes are real
   energy, and a speaker that cannot produce them is quieter. This is also true
   of the device. Tools > Sound > Volume can increase the level of the result.

   A person tuned these values by ear against the sound of the hardware. Nobody
   measured them at the speaker of a real unit. These are presets, and not a free
   cutoff control, because the real question is "which of these sounds most like
   the device on your speakers". The numbers are here for a person who wants to
   tune them again. */
#define SPEAKER_SIM_OFF 0
#define SPEAKER_SIM_LIGHT 1
#define SPEAKER_SIM_POCKETSTATION 2
#define SPEAKER_SIM_TINNY 3
#define SPEAKER_SIM_COUNT 4
/* master_volume has a default that makes no change to the signal, but this
   setting does not. The raw signal is the signal that does not agree with the
   hardware. Thus a new installation gets the speaker that this app emulates.
   The Full Range preset is available for a person who prefers the unchanged
   DAC output. */
#define SPEAKER_SIM_DEFAULT SPEAKER_SIM_POCKETSTATION

/* The index of this table is a SPEAKER_SIM_* value. `name` is the token in
   settings.cfg. The order here is also the order of the Tools > Sound > Speaker
   menu (see the ID_TOOLS_SPEAKER_BASE range in resource.h). The OFF row has no
   coefficients: no code reads them. That row keeps the same index values for
   this table as for each other table.

   `trim` is a small level adjustment that this code applies with the
   coefficients. It is not a makeup gain that tries to replace the energy that
   the high-pass filter removes. More gain into the limiter below only
   compresses the signal. The function of `trim` is to make the three presets
   approximately equal in loudness. That level measures at approximately -4dB
   from the raw signal, for all three presets. */
static const struct {
    const char *name;
    double cutoff_hz;
    double q;
    double trim;
} SPEAKER_SIM_PRESETS[SPEAKER_SIM_COUNT] = {
    {"off", 0.0, 0.0, 1.0},
    /* Removes the low-frequency energy, with only a small change to the
       character of the sound. This preset is for a person with small speakers,
       or a person who finds the other two presets too thin. */
    {"light", 500.0, 0.70, 1.10},
    /* The device. The corner frequency is immediately below the resonance of a
       transducer of that size. Q is sufficient to keep the peak audible. */
    {"pocketstation", 1100.0, 1.10, 1.20},
    /* This preset removes more low-frequency energy than the hardware does. It
       is for large speakers or a subwoofer, where the default preset still gives
       more low-frequency output than the device could produce. */
    {"tinny", 1800.0, 1.60, 1.10},
};

static int clamp_speaker_sim(int preset) {
    if (preset < 0 || preset >= SPEAKER_SIM_COUNT) {
        return SPEAKER_SIM_DEFAULT;
    }
    return preset;
}

/* Reads a speaker= token from settings.cfg. For a token that this code does not
   recognize, it uses the default preset. It does not use Off. Such a token comes
   from a manual edit, or from a later version with more presets. A value that
   this code cannot read must not turn the function off without a message. */
static int speaker_sim_from_name(const char *name) {
    for (int i = 0; i < SPEAKER_SIM_COUNT; i++) {
        if (strcmp(name, SPEAKER_SIM_PRESETS[i].name) == 0) {
            return i;
        }
    }
    return SPEAKER_SIM_DEFAULT;
}

/* The state of a Direct Form I biquad filter, and the preset of its
   coefficients. The history must continue across frames: the main loop filters
   the samples of one frame at a time. A filter that starts at zero each 31ms
   makes a click at each frame boundary. */
typedef struct {
    int preset; /* a SPEAKER_SIM_* value, or -1 before the first configuration */
    double b0, b1, b2, a1, a2;
    double x1, x2, y1, y2;
} speaker_filter_t;

static void speaker_filter_init(speaker_filter_t *f) {
    memset(f, 0, sizeof(*f));
    f->preset = -1;
}

/* M_PI in math.h is not part of standard C, and MSVC defines it only with
   _USE_MATH_DEFINES. Thus this code gives the constant directly. */
#define SPEAKER_TWO_PI 6.283185307179586

/* Calculates the coefficients again, but only when the preset changes. The main
   loop calls this function at each frame, with the current menu value.
   At a change, this function clears the history. Thus the new filter starts from
   silence, and not from the samples of the previous filter. */
static void speaker_filter_configure(speaker_filter_t *f, int preset) {
    if (f->preset == preset) {
        return;
    }
    f->preset = preset;
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0;
    if (preset == SPEAKER_SIM_OFF) {
        return;
    }
    /* An RBJ audio-EQ-cookbook high-pass filter, normalized by a0. The trim value
       is part of the feed-forward coefficients, thus it has no cost for each
       sample. */
    double w0 = SPEAKER_TWO_PI * SPEAKER_SIM_PRESETS[preset].cutoff_hz / (double)PSEMU_AUDIO_SAMPLE_RATE_HZ;
    double cos_w0 = cos(w0);
    double alpha = sin(w0) / (2.0 * SPEAKER_SIM_PRESETS[preset].q);
    double a0 = 1.0 + alpha;
    double gain = SPEAKER_SIM_PRESETS[preset].trim;
    f->b0 = gain * ((1.0 + cos_w0) / 2.0) / a0;
    f->b1 = gain * (-(1.0 + cos_w0)) / a0;
    f->b2 = f->b0;
    f->a1 = (-2.0 * cos_w0) / a0;
    f->a2 = (1.0 - alpha) / a0;
}

/* The level where the soft limiter below starts to change the signal, as a
   fraction of full scale. Each value below this level passes through with no
   change. */
#define SPEAKER_LIMIT_KNEE 0.75

/* A high-pass filter differentiates a step. Thus a full-scale square wave, which
   is almost the only signal that this DAC produces (see dac.h), leaves the
   filter with an overshoot to approximately two times full scale at each edge.
   This occurs before any gain. That behavior is part of the filter shape. A
   change to the coefficients cannot remove it, and the extra amplitude must go
   somewhere.

   A hard limit makes those peaks into square shapes. That result is the exact
   harsh digital clipping that this function must prevent. An attenuation of the
   full signal by a sufficient quantity, which is approximately -6dB, uses real
   loudness for transients that are only one or two samples wide.

   Thus this code folds the peaks instead. The response is linear below the knee,
   and it uses tanh above the knee. The tanh function is asymptotic to full
   scale, thus it can never overflow. It is also the most accurate of the three
   methods, because it is the behavior of the real speaker: a driver of that size
   reaches its excursion limit and compresses the peaks. It does not reproduce
   them. A measurement over square waves from 220Hz to 2.2kHz shows that this
   method costs less than 0.5dB of level against no limit. Thus the high-pass
   filter, and not this limiter, causes almost all of the approximately 4dB
   between the presets and the raw signal. */
static double speaker_soft_limit(double y) {
    double u = y / 32768.0;
    double magnitude = fabs(u);
    if (magnitude <= SPEAKER_LIMIT_KNEE) {
        return y;
    }
    double over = (magnitude - SPEAKER_LIMIT_KNEE) / (1.0 - SPEAKER_LIMIT_KNEE);
    double folded = SPEAKER_LIMIT_KNEE + (1.0 - SPEAKER_LIMIT_KNEE) * tanh(over);
    return (u < 0.0 ? -folded : folded) * 32768.0;
}

/* Filters the samples of one frame in the same buffer. The Off preset returns and
   does not change the buffer, thus that path costs only the preset comparison.
   At 8kHz there are approximately 250 samples for each frame. Thus the
   double-precision arithmetic for each sample is not significant against each
   other operation in a frame. */
static void apply_speaker_filter(speaker_filter_t *f, int16_t *samples, uint32_t count, int preset) {
    speaker_filter_configure(f, clamp_speaker_sim(preset));
    if (f->preset == SPEAKER_SIM_OFF) {
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        double x = (double)samples[i];
        double y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
        /* The history receives the value of y from before the limiter. This is
           deliberate. If the history receives the limited value, the
           non-linearity is inside the recursion. The result is then no longer a
           filter with a known response. */
        f->x2 = f->x1;
        f->x1 = x;
        f->y2 = f->y1;
        f->y1 = y;
        samples[i] = (int16_t)speaker_soft_limit(y);
    }
}

/* Combines 8-bit R, G, and B values into the same 0xRRGGBBAA layout that
   render_framebuffer writes into the pixel buffer. See the comment on
   render_framebuffer for the byte order of SDL_PIXELFORMAT_RGBA8888. The alpha
   value is always opaque. */
#define RGBA_PACK(r, g, b) \
    ((((uint32_t)(r)) << 24) | (((uint32_t)(g)) << 16) | (((uint32_t)(b)) << 8) | 0xFFu)

#define DISPLAY_PIXEL_LIGHT RGBA_PACK(0x00, 0x00, 0x00)
#define DISPLAY_BG_LIGHT RGBA_PACK(0xFF, 0xFF, 0xFF)
#define DISPLAY_PIXEL_DARK RGBA_PACK(0xFF, 0xFF, 0xFF)
#define DISPLAY_BG_DARK RGBA_PACK(0x00, 0x00, 0x00)
/* An approximation of a reflective or transflective LCD with no backlight, for
   example the display of a watch.
   The pixel color is a dark ink color with a small quantity of warmth. It is not
   pure black.
   The background color is a light sage-gray. It is not white.
   This is the default color scheme for a new settings.cfg file.
   See load_settings. */
#define DISPLAY_PIXEL_CLASSIC RGBA_PACK(0x11, 0x1A, 0x15)
#define DISPLAY_BG_CLASSIC RGBA_PACK(0xBC, 0xC7, 0xB9)

/* A real STN passive-matrix LCD from the late 1990s, which includes watches and
   the PocketStation, shows a small quantity of "ghosting" after a lit pixel.
   The cause is the slow response of the liquid crystal. It is not a drop shadow.
   This is the ghosting color of a new settings.cfg file. It agrees with the
   Classic scheme, which is also the initial scheme.
   The color is not fixed: when a user changes the color scheme, this app
   calculates the ghosting color for the new scheme (see theme_shadow_for). A
   ghost is a pixel of that scheme at one half of its intensity, and not a
   separate color.
   This value is exactly the value that theme_shadow_for returns for the Classic
   scheme. Thus a new settings.cfg file and a manual selection of
   View > Colors > Classic agree byte for byte. The earlier value was 8E9B8E,
   which a person selected manually and independently of the rule. That value is
   within one rounding step of this value. The two are the same color on the
   screen, but they leave two different values on disk. */
#define DISPLAY_SHADOW_COLOR RGBA_PACK(0x90, 0x9A, 0x8E)

/* The inverse of the top 3 bytes of RGBA_PACK. It formats the value in the form
   that settings.cfg uses for a color ("RRGGBB"). See save_settings. */
static void format_rgba_hex(uint32_t rgba, char *out, size_t out_size) {
    snprintf(out, out_size, "%02X%02X%02X", (unsigned)(rgba >> 24) & 0xFFu, (unsigned)(rgba >> 16) & 0xFFu,
        (unsigned)(rgba >> 8) & 0xFFu);
}

/* Returns a nonzero value if `path` already existed, and this function read it.
   Returns 0 if the file did not exist. A caller uses this result to tell "this
   is the first run, and there is nothing to read" from "the file exists, but it
   does not set each field". */
static int load_settings(app_settings_t *settings, const char *path) {
    FILE *f = fopen(path, "r");
    char line[1200];
    int existed = f != NULL;
    settings->bios_path[0] = '\0';
    settings->hardware_id[0] = '\0';
    settings->pixel_color[0] = '\0';
    settings->bg_color[0] = '\0';
    settings->shadow_color[0] = '\0';
    settings->key_up[0] = '\0';
    settings->key_down[0] = '\0';
    settings->key_left[0] = '\0';
    settings->key_right[0] = '\0';
    settings->key_fire[0] = '\0';
    settings->key_debug_log[0] = '\0';
    settings->key_reset[0] = '\0';
    settings->key_quick_save[0] = '\0';
    settings->key_quick_load[0] = '\0';
    settings->show_console = 0;
    settings->ir_link_diagnostics = 0;
    settings->show_shadows = 0;
    /* The default value is off. Thus a new installation operates the same way as
       before this override existed. */
    settings->datetime_override = DATETIME_OVERRIDE_OFF;
    /* Full volume. Thus an installation where nobody changed
       Tools > Sound > Volume sounds the same way as before this setting
       existed. */
    settings->master_volume = MASTER_VOLUME_DEFAULT;
    /* master_volume has a default that makes no change, but this default does
       make a change. See SPEAKER_SIM_DEFAULT for the reason. */
    settings->speaker_sim = SPEAKER_SIM_DEFAULT;
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            if (strncmp(line, "bios=", 5) == 0) {
                snprintf(settings->bios_path, sizeof(settings->bios_path), "%s", line + 5);
            } else if (strncmp(line, "hardware_id=", 12) == 0) {
                snprintf(settings->hardware_id, sizeof(settings->hardware_id), "%s", line + 12);
            } else if (strncmp(line, "pixel_color=", 12) == 0) {
                snprintf(settings->pixel_color, sizeof(settings->pixel_color), "%s", line + 12);
            } else if (strncmp(line, "bg_color=", 9) == 0) {
                snprintf(settings->bg_color, sizeof(settings->bg_color), "%s", line + 9);
            } else if (strncmp(line, "shadow_color=", 13) == 0) {
                snprintf(settings->shadow_color, sizeof(settings->shadow_color), "%s", line + 13);
            } else if (strncmp(line, "key_up=", 7) == 0) {
                snprintf(settings->key_up, sizeof(settings->key_up), "%s", line + 7);
            } else if (strncmp(line, "key_down=", 9) == 0) {
                snprintf(settings->key_down, sizeof(settings->key_down), "%s", line + 9);
            } else if (strncmp(line, "key_left=", 9) == 0) {
                snprintf(settings->key_left, sizeof(settings->key_left), "%s", line + 9);
            } else if (strncmp(line, "key_right=", 10) == 0) {
                snprintf(settings->key_right, sizeof(settings->key_right), "%s", line + 10);
            } else if (strncmp(line, "key_fire=", 9) == 0) {
                snprintf(settings->key_fire, sizeof(settings->key_fire), "%s", line + 9);
            } else if (strncmp(line, "key_debug_log=", 14) == 0) {
                snprintf(settings->key_debug_log, sizeof(settings->key_debug_log), "%s", line + 14);
            } else if (strncmp(line, "key_reset=", 10) == 0) {
                snprintf(settings->key_reset, sizeof(settings->key_reset), "%s", line + 10);
            } else if (strncmp(line, "key_quick_save=", 15) == 0) {
                snprintf(settings->key_quick_save, sizeof(settings->key_quick_save), "%s", line + 15);
            } else if (strncmp(line, "key_quick_load=", 15) == 0) {
                snprintf(settings->key_quick_load, sizeof(settings->key_quick_load), "%s", line + 15);
            } else if (strncmp(line, "datetime_override=", 18) == 0) {
                settings->datetime_override =
                    strcmp(line + 18, "os") == 0 ? DATETIME_OVERRIDE_OS : DATETIME_OVERRIDE_OFF;
            } else if (strncmp(line, "master_volume=", 14) == 0) {
                /* This code limits the value. It does not refuse the value. A
                   manually edited file with the value 150 means "the maximum
                   loudness". No value here is sufficiently incorrect to prevent
                   a start. */
                settings->master_volume = clamp_master_volume(atoi(line + 14));
            } else if (strncmp(line, "speaker=", 8) == 0) {
                settings->speaker_sim = speaker_sim_from_name(line + 8);
            } else if (strncmp(line, "show_console=", 13) == 0) {
                settings->show_console = atoi(line + 13) != 0;
            } else if (strncmp(line, "ir_link_diagnostics=", 20) == 0) {
                settings->ir_link_diagnostics = atoi(line + 20) != 0;
            } else if (strncmp(line, "show_shadows=", 13) == 0) {
                settings->show_shadows = atoi(line + 13) != 0;
            }
        }
        fclose(f);
    }
    /* This code covers two conditions: settings.cfg does not exist, or the file
       exists but a program wrote it before one of these fields existed.
       Write the true default value into each field that is empty:
       PSEMU_DEFAULT_HARDWARE_ID, the Classic color scheme, the default shadow
       color, and the original key bindings.
       Leave no field empty.
       Thus the file always shows the values that are in effect. It does not use
       a default value that the file gives no data about. */
    if (settings->hardware_id[0] == '\0') {
        psemu_format_hardware_id(PSEMU_DEFAULT_HARDWARE_ID, settings->hardware_id, sizeof(settings->hardware_id));
    }
    if (settings->pixel_color[0] == '\0') {
        format_rgba_hex(DISPLAY_PIXEL_CLASSIC, settings->pixel_color, sizeof(settings->pixel_color));
    }
    if (settings->bg_color[0] == '\0') {
        format_rgba_hex(DISPLAY_BG_CLASSIC, settings->bg_color, sizeof(settings->bg_color));
    }
    if (settings->shadow_color[0] == '\0') {
        format_rgba_hex(DISPLAY_SHADOW_COLOR, settings->shadow_color, sizeof(settings->shadow_color));
    }
    if (settings->key_up[0] == '\0') {
        snprintf(settings->key_up, sizeof(settings->key_up), "%s", SDL_GetScancodeName(SDL_SCANCODE_UP));
    }
    if (settings->key_down[0] == '\0') {
        snprintf(settings->key_down, sizeof(settings->key_down), "%s", SDL_GetScancodeName(SDL_SCANCODE_DOWN));
    }
    if (settings->key_left[0] == '\0') {
        snprintf(settings->key_left, sizeof(settings->key_left), "%s", SDL_GetScancodeName(SDL_SCANCODE_LEFT));
    }
    if (settings->key_right[0] == '\0') {
        snprintf(settings->key_right, sizeof(settings->key_right), "%s", SDL_GetScancodeName(SDL_SCANCODE_RIGHT));
    }
    if (settings->key_fire[0] == '\0') {
        snprintf(settings->key_fire, sizeof(settings->key_fire), "%s", SDL_GetScancodeName(SDL_SCANCODE_Z));
    }
    if (settings->key_debug_log[0] == '\0') {
        snprintf(settings->key_debug_log, sizeof(settings->key_debug_log), "%s", SDL_GetScancodeName(SDL_SCANCODE_F12));
    }
    if (settings->key_reset[0] == '\0') {
        snprintf(settings->key_reset, sizeof(settings->key_reset), "%s", SDL_GetScancodeName(SDL_SCANCODE_F8));
    }
    if (settings->key_quick_save[0] == '\0') {
        snprintf(settings->key_quick_save, sizeof(settings->key_quick_save), "%s", SDL_GetScancodeName(SDL_SCANCODE_F5));
    }
    if (settings->key_quick_load[0] == '\0') {
        snprintf(settings->key_quick_load, sizeof(settings->key_quick_load), "%s", SDL_GetScancodeName(SDL_SCANCODE_F9));
    }
    return existed;
}

static void save_settings(const app_settings_t *settings, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "psemu: failed to persist settings to %s\n", path);
        return;
    }
    fprintf(f, "bios=%s\n", settings->bios_path);
    fprintf(f, "hardware_id=%s\n", settings->hardware_id);
    fprintf(f, "pixel_color=%s\n", settings->pixel_color);
    fprintf(f, "bg_color=%s\n", settings->bg_color);
    fprintf(f, "shadow_color=%s\n", settings->shadow_color);
    fprintf(f, "key_up=%s\n", settings->key_up);
    fprintf(f, "key_down=%s\n", settings->key_down);
    fprintf(f, "key_left=%s\n", settings->key_left);
    fprintf(f, "key_right=%s\n", settings->key_right);
    fprintf(f, "key_fire=%s\n", settings->key_fire);
    fprintf(f, "key_debug_log=%s\n", settings->key_debug_log);
    fprintf(f, "key_reset=%s\n", settings->key_reset);
    fprintf(f, "key_quick_save=%s\n", settings->key_quick_save);
    fprintf(f, "key_quick_load=%s\n", settings->key_quick_load);
    fprintf(f, "datetime_override=%s\n", settings->datetime_override == DATETIME_OVERRIDE_OS ? "os" : "default");
    fprintf(f, "master_volume=%d\n", settings->master_volume);
    fprintf(f, "speaker=%s\n", SPEAKER_SIM_PRESETS[clamp_speaker_sim(settings->speaker_sim)].name);
    fprintf(f, "show_console=%d\n", settings->show_console ? 1 : 0);
    fprintf(f, "ir_link_diagnostics=%d\n", settings->ir_link_diagnostics ? 1 : 0);
    fprintf(f, "show_shadows=%d\n", settings->show_shadows ? 1 : 0);
    fclose(f);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* Reads exactly 6 hex digits ("RRGGBB") and nothing else.
   This rule is deliberately strict, and it permits no other format. It agrees
   with psemu_parse_hardware_id in psemu.h. */
static int parse_hex_rgb(const char *s, uint8_t *r, uint8_t *g, uint8_t *b) {
    int nibbles[6];
    int i;
    if (!s || strlen(s) != 6) {
        return 0;
    }
    for (i = 0; i < 6; i++) {
        nibbles[i] = hex_nibble(s[i]);
        if (nibbles[i] < 0) {
            return 0;
        }
    }
    *r = (uint8_t)((nibbles[0] << 4) | nibbles[1]);
    *g = (uint8_t)((nibbles[2] << 4) | nibbles[3]);
    *b = (uint8_t)((nibbles[4] << 4) | nibbles[5]);
    return 1;
}

/* One entry in the map from a key to a PocketStation button.
   The main loop reads this map at each frame (see button_scancodes in main).
   Only the remap dialog and its labels use display_name. This app never keeps
   display_name in a file. It keeps the SDL_GetScancodeName value of the scancode
   in settings.cfg.
   `bit` is 0 for the last entry, "Create Debug Log".
   That entry is not a PocketStation button.
   See button_scancodes in main for the use of that entry. */
typedef struct {
    SDL_Scancode scancode;
    uint32_t bit;
    const char *display_name;
} button_binding_t;

/* The number of rows in Tools > Remap Controls: Up, Down, Left, Right, and Fire,
   and then 4 hotkeys that are not buttons: Create Debug Log, Reset, Save State,
   and Load State.
   This value agrees with the row count of IDD_REMAP_CONTROLS.
   It also agrees with the IDC_REMAP_LABEL_BASE and IDC_REMAP_CHANGE_BASE ranges
   in resource.h. Each of those ranges has 9 sequential IDs. */
#define REMAP_BINDING_COUNT 9

/* This app writes this marker into a key_* field in settings.cfg, in place of a
   real key name. It does this when a user clears that row, because a different
   row now uses its key (see prompt_remap_controls).
   This marker is different from an empty field. An empty field means "a user
   never set this row, thus use the default key" (see resolve_key_binding).
   Without this difference, this app writes an unbound row as "". The row then
   returns to its default key at the next start, and gives no message. */
#define KEY_BINDING_UNBOUND_MARKER "(unbound)"

/* `saved_name` is a key_* field from settings.cfg.
   This function reads it with SDL_GetScancodeFromName.
   It uses `fallback` if `saved_name` is empty, or if `saved_name` is not the name
   of a real key. An incorrect manual edit is one cause of an invalid name.
   For a field that a user cleared (see KEY_BINDING_UNBOUND_MARKER), this function
   returns SDL_SCANCODE_UNKNOWN. It does not use `fallback`.
   That result is a deliberate "no key" state. It is not a missing or invalid
   state. */
static SDL_Scancode resolve_key_binding(const char *saved_name, SDL_Scancode fallback) {
    if (strcmp(saved_name, KEY_BINDING_UNBOUND_MARKER) == 0) {
        return SDL_SCANCODE_UNKNOWN;
    }
    if (saved_name[0] != '\0') {
        SDL_Scancode sc = SDL_GetScancodeFromName(saved_name);
        if (sc != SDL_SCANCODE_UNKNOWN) {
            return sc;
        }
    }
    return fallback;
}

/* SDL_GetScancodeName(SDL_SCANCODE_UNKNOWN) returns "".
   In settings.cfg, "" is the same as "a user never set this row" (see
   resolve_key_binding).
   Thus this function writes KEY_BINDING_UNBOUND_MARKER for a row that a user
   cleared. */
static void format_key_binding_name(char *out, size_t out_size, SDL_Scancode scancode) {
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        snprintf(out, out_size, "%s", KEY_BINDING_UNBOUND_MARKER);
    } else {
        snprintf(out, out_size, "%s", SDL_GetScancodeName(scancode));
    }
}

/* The inverse of resolve_key_binding.
   `bindings` must hold exactly REMAP_BINDING_COUNT entries. The order is fixed:
   Up, Down, Left, Right, Fire, Create Debug Log, Reset, Save State, and Load
   State (see button_scancodes in main). */
static void save_key_bindings(app_settings_t *settings, const button_binding_t bindings[REMAP_BINDING_COUNT]) {
    format_key_binding_name(settings->key_up, sizeof(settings->key_up), bindings[0].scancode);
    format_key_binding_name(settings->key_down, sizeof(settings->key_down), bindings[1].scancode);
    format_key_binding_name(settings->key_left, sizeof(settings->key_left), bindings[2].scancode);
    format_key_binding_name(settings->key_right, sizeof(settings->key_right), bindings[3].scancode);
    format_key_binding_name(settings->key_fire, sizeof(settings->key_fire), bindings[4].scancode);
    format_key_binding_name(settings->key_debug_log, sizeof(settings->key_debug_log), bindings[5].scancode);
    format_key_binding_name(settings->key_reset, sizeof(settings->key_reset), bindings[6].scancode);
    format_key_binding_name(settings->key_quick_save, sizeof(settings->key_quick_save), bindings[7].scancode);
    format_key_binding_name(settings->key_quick_load, sizeof(settings->key_quick_load), bindings[8].scancode);
}

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

/* The state that the WM_COMMAND handlers of the menu bar must access.
   This structure collects that state, because SDL_SetWindowsMessageHook accepts
   only one void *userdata pointer. */
typedef struct {
    psemu_t *ps;
    uint8_t **bios;
    size_t *bios_size;
    char *bios_path;
    size_t bios_path_cap;
    uint8_t **app;
    size_t *app_size;
    char *app_path;
    size_t app_path_cap;
    HWND hwnd;
    int *running;
    int *cpu_faulted_reported;
    uint32_t *pixel_rgba;
    uint32_t *bg_rgba;
    int *show_shadows;
    uint32_t *shadow_rgba;
    button_binding_t *button_scancodes; /* an array of REMAP_BINDING_COUNT entries, in the fixed order Up, Down, Left, Right, Fire, Debug Log, Reset, Save State, Load State. See main. */
    app_settings_t *settings;
    const char *settings_path;
    const char *exe_dir;
    ir_link_t *ir_link;
    content_writeback_t *content_writeback;
} menu_context_t;

/* Shows the live status of the IR link as the full window title, for example
   "IR - Connected". This title replaces the usual "pokketstation" title while the link is active. The
   text is deliberately short: the default window is small, thus Windows cuts a longer title. The status
   is the one item that a user must see quickly.
   The hosting state and the connecting state get their own short strings. They do not use the text of
   ir_link_status_text, because ir_link_selftest.c and ir_probe also use that text for console output.
   That text can hold more detail than this title permits. The connected state and the error states use
   status_text without a change: "Connected" is already short, and it also contains the live counters if
   ir_link_diagnostics is on. The detail of an error message is also necessary.
   Each action that can change the state of ir_link calls this function. Those actions are Host,
   Connect, and Disconnect in the menu.
   The main loop also calls this function one time for each frame. A link that hosts or connects can
   change its state without a menu selection: a peer connects, or the link fails. */
static void ir_link_refresh_title(menu_context_t *ctx) {
    char title[192];
    if (!ir_link_is_active(ctx->ir_link)) {
        snprintf(title, sizeof(title), "pokketstation");
    } else if (ctx->ir_link->state == IR_LINK_HOSTING) {
        snprintf(title, sizeof(title), "IR - Waiting...");
    } else if (ctx->ir_link->state == IR_LINK_CONNECTING) {
        snprintf(title, sizeof(title), "IR - Connecting...");
    } else {
        snprintf(title, sizeof(title), "IR - %s", ir_link_status_text(ctx->ir_link));
    }
    SetWindowTextA(ctx->hwnd, title);
}

/* psemu_reset and psemu_load_state both clear the clock of ir_t and each edge in its queues (see the
   comment on ir_link_disconnect in ir_link.h). A link that stays connected through either call loses
   synchronization and gives no error: the IR clock of this instance returns to zero, and the clock of
   the peer does not. Thus each subsequent timestamp is incorrect. Each function that resets the
   emulator or loads a state calls this function first. */
static void drop_ir_link_if_active(menu_context_t *ctx) {
    if (ir_link_is_active(ctx->ir_link)) {
        ir_link_disconnect(ctx->ir_link);
        ir_link_refresh_title(ctx);
    }
}

static void prompt_open_bios(menu_context_t *ctx) {
    char path[1024] = {0};
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ctx->hwnd;
    ofn.lpstrFilter = "BIOS dump (*.bin)\0*.bin\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.lpstrTitle = "Load BIOS dump";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) {
        return;
    }

    size_t new_size = 0;
    uint8_t *new_bios = read_file(path, &new_size);
    if (!new_bios) {
        MessageBoxA(ctx->hwnd, "Couldn't read that file.", "pokketstation", MB_ICONERROR);
        return;
    }
    if (psemu_load_bios(ctx->ps, new_bios, new_size) != PSEMU_OK) {
        MessageBoxA(ctx->hwnd, "Not a valid BIOS image (expected a 16384-byte dump).", "pokketstation", MB_ICONERROR);
        free(new_bios);
        return;
    }

    free(*ctx->bios);
    *ctx->bios = new_bios;
    *ctx->bios_size = new_size;
    snprintf(ctx->bios_path, ctx->bios_path_cap, "%s", path);
    psemu_reset(ctx->ps);
    drop_ir_link_if_active(ctx);
    *ctx->cpu_faulted_reported = 0;

    snprintf(ctx->settings->bios_path, sizeof(ctx->settings->bios_path), "%s", path);
    save_settings(ctx->settings, ctx->settings_path);
}

static void prompt_open_app(menu_context_t *ctx) {
    char path[1024] = {0};
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ctx->hwnd;
    ofn.lpstrFilter =
        "App or memory-card image (*.pss;*.mcs;*.mcr)\0*.pss;*.mcs;*.mcr\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.lpstrTitle = "Open app or memory card";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) {
        return;
    }

    size_t new_size = 0;
    uint8_t *new_app = read_file(path, &new_size);
    if (!new_app) {
        MessageBoxA(ctx->hwnd, "Couldn't read that file.", "pokketstation", MB_ICONERROR);
        return;
    }
    if (psemu_load_content(ctx->ps, new_app, new_size) != PSEMU_OK) {
        MessageBoxA(ctx->hwnd, "Not a valid app or memory-card image.", "pokketstation", MB_ICONERROR);
        free(new_app);
        return;
    }

    /* This call occurs before the code loses access to the file that it replaces. An open operation
       on a different file is not a reason to discard an edit that the app already made to the
       loaded content. */
    content_writeback_commit(ctx->content_writeback, ctx->ps);

    free(*ctx->app);
    *ctx->app = new_app;
    *ctx->app_size = new_size;
    snprintf(ctx->app_path, ctx->app_path_cap, "%s", path);
    psemu_reset(ctx->ps);
    content_writeback_arm(ctx->content_writeback, ctx->ps, path, new_app, new_size);
    drop_ir_link_if_active(ctx);
    *ctx->cpu_faulted_reported = 0;
}

/* Starts the loaded BIOS and the loaded app or card again, from a clean state.
   This function loads neither file again.
   psemu_reset does this same reset after a new load. See its comment in psemu.c.
   This function causes the reset when a user asks for it, and not only after a successful file
   dialog. */
static void reset_emulation(menu_context_t *ctx) {
    /* A reset does not change flash (see psemu_reset). Thus this code writes an edit that the app
       already made, and does not discard it. It then uses the remaining data as the new baseline. */
    content_writeback_commit(ctx->content_writeback, ctx->ps);
    psemu_reset(ctx->ps);
    content_writeback_resync(ctx->content_writeback, ctx->ps);
    drop_ir_link_if_active(ctx);
    *ctx->cpu_faulted_reported = 0;
}

static void save_state_to_slot(menu_context_t *ctx, int slot) {
    char path[1024];
    char msg[256];
    get_save_slot_path(path, sizeof(path), ctx->exe_dir, ctx->app_path, slot);

    size_t state_size = psemu_state_size(ctx->ps);
    size_t total_size = sizeof(quicksave_header_t) + state_size;
    uint8_t *buf = (uint8_t *)malloc(total_size);
    if (!buf) {
        MessageBoxA(ctx->hwnd, "Out of memory.", "pokketstation", MB_ICONERROR);
        return;
    }

    quicksave_header_t header;
    memcpy(header.magic, QUICKSAVE_MAGIC, 4);
    header.version = QUICKSAVE_VERSION;
    header.app_size = (uint32_t)*ctx->app_size;
    header.app_hash = psemu_content_identity_hash(*ctx->app, *ctx->app_size);
    memcpy(buf, &header, sizeof(header));
    psemu_save_state(ctx->ps, buf + sizeof(header), state_size);

    FILE *f = fopen(path, "wb");
    if (!f || fwrite(buf, 1, total_size, f) != total_size) {
        snprintf(msg, sizeof(msg), "Couldn't write the save state file for %s.", save_slot_label(slot));
        MessageBoxA(ctx->hwnd, msg, "pokketstation", MB_ICONERROR);
    }
    if (f) {
        fclose(f);
    }
    free(buf);
}

static void load_state_from_slot(menu_context_t *ctx, int slot) {
    char path[1024];
    char msg[256];
    get_save_slot_path(path, sizeof(path), ctx->exe_dir, ctx->app_path, slot);

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(msg, sizeof(msg), "No save state in %s for the currently loaded app/card.", save_slot_label(slot));
        MessageBoxA(ctx->hwnd, msg, "pokketstation", MB_ICONWARNING);
        return;
    }

    quicksave_header_t header;
    if (fread(&header, 1, sizeof(header), f) != sizeof(header) || memcmp(header.magic, QUICKSAVE_MAGIC, 4) != 0 ||
        header.version != QUICKSAVE_VERSION) {
        snprintf(msg, sizeof(msg), "The file for %s isn't a valid save state.", save_slot_label(slot));
        MessageBoxA(ctx->hwnd, msg, "pokketstation", MB_ICONERROR);
        fclose(f);
        return;
    }

    uint32_t current_hash = psemu_content_identity_hash(*ctx->app, *ctx->app_size);
    if (header.app_size != (uint32_t)*ctx->app_size || header.app_hash != current_hash) {
        snprintf(msg, sizeof(msg), "The save state in %s doesn't match the currently loaded app/card.",
            save_slot_label(slot));
        MessageBoxA(ctx->hwnd, msg, "pokketstation", MB_ICONERROR);
        fclose(f);
        return;
    }

    size_t state_size = psemu_state_size(ctx->ps);

    /* The state data must be exactly one state for THIS build, and only the
       length of the file can show this. psemu_load_state cannot show it: its
       size argument comes from psemu_state_size in this same function, thus the
       argument is always sizeof(psemu_t), and the test in that function can
       never fail. The short read below finds state data from a build with a
       smaller psemu_t. It does not find state data from a build with a larger
       psemu_t: the fread call is successful on the first bytes, and the code
       then loads a state with an incorrect alignment and gives no error.
       QUICKSAVE_VERSION must find both conditions, but it operates only if a
       person increases it at each change to psemu_t. This test does not depend
       on a person. */
    long body_start = ftell(f);
    if (body_start < 0 || fseek(f, 0, SEEK_END) != 0) {
        snprintf(msg, sizeof(msg), "Couldn't read the save state in %s.", save_slot_label(slot));
        MessageBoxA(ctx->hwnd, msg, "pokketstation", MB_ICONERROR);
        fclose(f);
        return;
    }
    long body_size = ftell(f) - body_start;
    if (body_size != (long)state_size || fseek(f, body_start, SEEK_SET) != 0) {
        snprintf(msg, sizeof(msg), "Couldn't load the save state in %s (wrong build/version?).",
            save_slot_label(slot));
        MessageBoxA(ctx->hwnd, msg, "pokketstation", MB_ICONERROR);
        fclose(f);
        return;
    }

    uint8_t *buf = (uint8_t *)malloc(state_size);
    if (!buf || fread(buf, 1, state_size, f) != state_size || psemu_load_state(ctx->ps, buf, state_size) != PSEMU_OK) {
        snprintf(msg, sizeof(msg), "Couldn't load the save state in %s (wrong build/version?).",
            save_slot_label(slot));
        MessageBoxA(ctx->hwnd, msg, "pokketstation", MB_ICONERROR);
        free(buf);
        fclose(f);
        return;
    }

    free(buf);
    fclose(f);
    /* A save state contains its own copy of flash. Thus the full card changed, and no app wrote to
       it. That change is not an edit to write to the file. Use it as the new baseline instead, and
       write only the changes that the app makes after this point. */
    content_writeback_resync(ctx->content_writeback, ctx->ps);
    drop_ir_link_if_active(ctx);
    *ctx->cpu_faulted_reported = 0;
}

/* The lParam data for hwid_dialog_proc.
   DialogBoxParamA supplies it, and GetWindowLongPtrA(GWLP_USERDATA) reads it.
   This code writes parsed_id, and permits IDOK to close the dialog, only after
   psemu_parse_hardware_id accepts the text in the edit control. */
typedef struct {
    char text[PSEMU_HARDWARE_ID_STRING_SIZE];
    uint32_t parsed_id;
} hwid_dialog_data_t;

static INT_PTR CALLBACK hwid_dialog_proc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtrA(hdlg, GWLP_USERDATA, (LONG_PTR)lparam);
        SetDlgItemTextA(hdlg, IDC_HWID_EDIT, ((hwid_dialog_data_t *)lparam)->text);
        /* PSEMU_HARDWARE_ID_STRING_SIZE includes the '\0' character.
           The canonical form is always exactly 8 hex digits.
           Limit the input to that length here. Do not let
           psemu_parse_hardware_id refuse the value later. */
        SendDlgItemMessageA(hdlg, IDC_HWID_EDIT, EM_SETLIMITTEXT, PSEMU_HARDWARE_ID_STRING_SIZE - 1, 0);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK) {
            hwid_dialog_data_t *data = (hwid_dialog_data_t *)GetWindowLongPtrA(hdlg, GWLP_USERDATA);
            char text[PSEMU_HARDWARE_ID_STRING_SIZE];
            GetDlgItemTextA(hdlg, IDC_HWID_EDIT, text, sizeof(text));
            if (!psemu_parse_hardware_id(text, &data->parsed_id)) {
                MessageBoxA(hdlg, "Expected exactly 8 hex digits (0-9, A-F).", "pokketstation", MB_ICONERROR);
                return TRUE;
            }
            EndDialog(hdlg, IDOK);
            return TRUE;
        } else if (LOWORD(wparam) == IDCANCEL) {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void prompt_edit_hardware_id(menu_context_t *ctx) {
    hwid_dialog_data_t data;
    psemu_format_hardware_id(psemu_get_hardware_id(ctx->ps), data.text, sizeof(data.text));
    if (DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_HWID), ctx->hwnd, hwid_dialog_proc,
            (LPARAM)&data) == IDOK) {
        psemu_set_hardware_id(ctx->ps, data.parsed_id);
        psemu_format_hardware_id(data.parsed_id, ctx->settings->hardware_id, sizeof(ctx->settings->hardware_id));
        save_settings(ctx->settings, ctx->settings_path);
    }
}

/* Changes the window size. Thus its *client* area, where this app renders the
   framebuffer, becomes exactly PSEMU_LCD_WIDTH * SCALE * multiplier by
   PSEMU_LCD_HEIGHT * SCALE * multiplier.
   This is true for each quantity of window decoration: the menu bar, the title
   bar, and the borders.
   This function uses the same GetClientRect method, before and after the change,
   that corrects for the height of the menu bar at startup. */
static void resize_client_to_scale(HWND hwnd, int multiplier) {
    RECT client, window_rect;
    int chrome_w, chrome_h, target_w, target_h;

    GetClientRect(hwnd, &client);
    GetWindowRect(hwnd, &window_rect);
    chrome_w = (window_rect.right - window_rect.left) - (client.right - client.left);
    chrome_h = (window_rect.bottom - window_rect.top) - (client.bottom - client.top);
    target_w = PSEMU_LCD_WIDTH * SCALE * multiplier;
    target_h = PSEMU_LCD_HEIGHT * SCALE * multiplier;
    SetWindowPos(hwnd, NULL, 0, 0, target_w + chrome_w, target_h + chrome_h, SWP_NOMOVE | SWP_NOZORDER);
}

/* --- The calculation of a full scheme from one selected color -------------
   View > Colors > Advanced Colors... asks for one color, the screen color
   (the background). It calculates the other two colors from that color.
   The background is the reference color, and not the active pixel color,
   because most of the LCD is unlit for most of the time. The background *is*
   the color that a user sees on the screen. A person also describes an LCD by
   the tint of its panel, and not by the color of its ink.
   The two calculated colors keep the hue of the selected color. This rule
   prevents an unpleasant result from any selection: the ink of a real STN LCD
   is a darker form of its panel color, and not a separate color. Only the
   lightness and the saturation change.
   A test of this rule: derive_theme_colors converts the Classic background
   (BCC7B9) to 20291D and 939E90. Those values are slightly softer than the
   manual Classic ink and shadow that this project used (111A15 and 909A8E).
   Thus the rule agrees with the selection of a person. */

/* Reverses the sRGB transfer function for one channel in the range 0 to 1. Thus
   this code can give a weight to each channel and calculate a true luminance. A
   simple average of the RGB values is not perceptual: full green looks much
   brighter than full blue at the same numeric value. */
static double srgb_to_linear(double c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

/* WCAG 2.x relative luminance of an RGBA_PACK value (alpha ignored). */
static double rgba_luminance(uint32_t rgba) {
    double r = srgb_to_linear((double)((rgba >> 24) & 0xFFu) / 255.0);
    double g = srgb_to_linear((double)((rgba >> 16) & 0xFFu) / 255.0);
    double b = srgb_to_linear((double)((rgba >> 8) & 0xFFu) / 255.0);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

/* WCAG 2.x contrast ratio, 1.0 (identical) to 21.0 (black on white). */
static double contrast_ratio(uint32_t a, uint32_t b) {
    double la = rgba_luminance(a) + 0.05;
    double lb = rgba_luminance(b) + 0.05;
    return la > lb ? la / lb : lb / la;
}

/* All three outputs are 0..1. Hue is a fraction of the circle, not degrees. */
static void rgba_to_hsl(uint32_t rgba, double *h, double *s, double *l) {
    double r = (double)((rgba >> 24) & 0xFFu) / 255.0;
    double g = (double)((rgba >> 16) & 0xFFu) / 255.0;
    double b = (double)((rgba >> 8) & 0xFFu) / 255.0;
    double max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    double min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double delta = max - min;
    *l = (max + min) / 2.0;
    if (delta <= 0.0) {
        /* A pure gray has no hue with a meaning. A result of 0 (red) has no bad
           effect: derive_theme_colors keeps the saturation at 0 for such a
           color. Thus the calculated colors also stay gray. */
        *h = 0.0;
        *s = 0.0;
        return;
    }
    /* Safe from a divide by zero: l is only 0.0 or 1.0 when max == min, which
       the delta check above already returned on. */
    *s = delta / (1.0 - fabs(2.0 * (*l) - 1.0));
    if (max == r) {
        *h = (g - b) / delta;
        if (*h < 0.0) {
            *h += 6.0;
        }
    } else if (max == g) {
        *h = (b - r) / delta + 2.0;
    } else {
        *h = (r - g) / delta + 4.0;
    }
    *h /= 6.0;
}

static uint8_t channel_to_byte(double v) {
    int scaled = (int)(v * 255.0 + 0.5);
    if (scaled < 0) {
        scaled = 0;
    }
    if (scaled > 255) {
        scaled = 255;
    }
    return (uint8_t)scaled;
}

static double hue_to_channel(double p, double q, double t) {
    if (t < 0.0) {
        t += 1.0;
    }
    if (t > 1.0) {
        t -= 1.0;
    }
    if (t < 1.0 / 6.0) {
        return p + (q - p) * 6.0 * t;
    }
    if (t < 1.0 / 2.0) {
        return q;
    }
    if (t < 2.0 / 3.0) {
        return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    }
    return p;
}

/* Inverse of rgba_to_hsl. Hue is expected in 0..1 (it wraps), s and l in 0..1. */
static uint32_t hsl_to_rgba(double h, double s, double l) {
    double r, g, b;
    if (s <= 0.0) {
        r = g = b = l;
    } else {
        double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
        double p = 2.0 * l - q;
        r = hue_to_channel(p, q, h + 1.0 / 3.0);
        g = hue_to_channel(p, q, h);
        b = hue_to_channel(p, q, h - 1.0 / 3.0);
    }
    return RGBA_PACK(channel_to_byte(r), channel_to_byte(g), channel_to_byte(b));
}

/* Mixes `t` (0..1) of `to` into `from`, per channel. */
static uint32_t blend_rgba(uint32_t from, uint32_t to, double t) {
    uint8_t out[3];
    int i;
    for (i = 0; i < 3; i++) {
        int shift = 24 - i * 8;
        double a = (double)((from >> shift) & 0xFFu);
        double b = (double)((to >> shift) & 0xFFu);
        out[i] = channel_to_byte((a + (b - a) * t) / 255.0);
    }
    return RGBA_PACK(out[0], out[1], out[2]);
}

/* The calculated ink color must get to this contrast ratio against the
   background, for each selected color that physically permits it. A ratio of
   8.5:1 is above the AAA level (7:1), but it is not the maximum possible
   contrast. For comparison, the Classic scheme has a ratio of approximately
   10:1. */
#define THEME_INK_MIN_CONTRAST 8.5
/* Contrast alone is not sufficient to make the ink *look* like ink. A light
   background can meet the contrast target at a lightness of 34%. But that
   result looks like gray on white, and not like a display. Thus this code makes
   dark ink at least this dark, and light ink at least this light. */
#define THEME_INK_DARK_MAX_LIGHTNESS 0.30
#define THEME_INK_LIGHT_MIN_LIGHTNESS 0.78
/* The ghosting trail is a pixel with a partial change of state. Thus its color
   is between the background color and the ink color, and much nearer to the
   background. The value 0.26 is not arbitrary: it is the position of the manual
   Classic ghosting color (8E9B8E) between the background and the ink of that
   scheme, to within one rounding step. A person selected that color before this
   rule existed. Thus this rule agrees with the selection of a person. */
#define THEME_SHADOW_MIX 0.26

/* The one rule for the ghosting trail of a scheme. The Light, Dark, and Classic
   presets use this rule, and the calculated scheme of Advanced Colors... also
   uses it. Thus no path can become different from the others.
   DISPLAY_SHADOW_COLOR is the result of this function for the Classic scheme.
   That value is a constant, because this app writes the default values of
   settings.cfg before a user selects a scheme. */
static uint32_t theme_shadow_for(uint32_t bg_rgba, uint32_t pixel_rgba) {
    return blend_rgba(bg_rgba, pixel_rgba, THEME_SHADOW_MIX);
}

/* Calculates the active pixel color and the sprite shadow color for `bg_rgba`.
   Neither result is ever equal to the input.
   The caller keeps the selected background color without a change. This code
   never "corrects" that color. Thus a deliberately bright selection still gets a
   usable scheme, and not a different color with no message. */
static void derive_theme_colors(uint32_t bg_rgba, uint32_t *out_pixel, uint32_t *out_shadow) {
    double h, s, l, ink_s, ink_l, step;
    /* The ink moves in the direction with more available range. This code
       compares against both limits. It does not test for a lightness of more
       than 50%. That method gives the correct result for a mid-tone selection: a
       saturated middle blue has more available range towards white than the
       simple lightness test shows. */
    int dark_ink = contrast_ratio(bg_rgba, RGBA_PACK(0x00, 0x00, 0x00))
        >= contrast_ratio(bg_rgba, RGBA_PACK(0xFF, 0xFF, 0xFF));
    int i;

    rgba_to_hsl(bg_rgba, &h, &s, &l);
    if (dark_ink) {
        /* The saturation must increase for dark ink. If it does not, the hue
           becomes black. A dark tone that is near to gray makes a scheme look
           unclear. */
        ink_s = s * 1.5 > 0.45 ? 0.45 : s * 1.5;
        step = -0.005;
    } else {
        /* Light ink is the opposite condition: the full saturation of the
           background in a tone that is near to white looks too bright. */
        ink_s = s * 0.6 > 0.30 ? 0.30 : s * 0.6;
        step = 0.005;
    }
    /* Move away from the lightness of the background, and stop at the first step
       that meets the contrast target. Do not go directly to black or to white.
       An early stop is the purpose of this loop: the softest ink that a user can
       still read is the ink that agrees best with the background.
       The search starts at the extreme end. Thus it also covers a selection where
       the target is physically impossible. For example, a saturated mid-tone red
       screen has a maximum ratio of approximately 5:1. Such a selection gets the
       best available contrast. */
    ink_l = dark_ink ? 0.0 : 1.0;
    for (i = 0; i <= 200; i++) {
        double candidate = l + step * (double)i;
        if (candidate < 0.0 || candidate > 1.0) {
            break;
        }
        if (contrast_ratio(hsl_to_rgba(h, ink_s, candidate), bg_rgba) >= THEME_INK_MIN_CONTRAST) {
            ink_l = candidate;
            break;
        }
    }
    if (dark_ink && ink_l > THEME_INK_DARK_MAX_LIGHTNESS) {
        ink_l = THEME_INK_DARK_MAX_LIGHTNESS;
    }
    if (!dark_ink && ink_l < THEME_INK_LIGHT_MIN_LIGHTNESS) {
        ink_l = THEME_INK_LIGHT_MIN_LIGHTNESS;
    }
    *out_pixel = hsl_to_rgba(h, ink_s, ink_l);
    *out_shadow = theme_shadow_for(bg_rgba, *out_pixel);
}

static void apply_display_colors_full(menu_context_t *ctx, uint32_t pixel_rgba, uint32_t bg_rgba,
    uint32_t shadow_rgba) {
    *ctx->pixel_rgba = pixel_rgba;
    *ctx->bg_rgba = bg_rgba;
    *ctx->shadow_rgba = shadow_rgba;
    format_rgba_hex(pixel_rgba, ctx->settings->pixel_color, sizeof(ctx->settings->pixel_color));
    format_rgba_hex(bg_rgba, ctx->settings->bg_color, sizeof(ctx->settings->bg_color));
    format_rgba_hex(shadow_rgba, ctx->settings->shadow_color, sizeof(ctx->settings->shadow_color));
    save_settings(ctx->settings, ctx->settings_path);
}

/* A change to a full scheme, which is the Light, Dark, or Classic preset,
   calculates the sprite shadow color again. Advanced Colors... does the same
   operation. A scheme is all three colors: the sage-gray ghost of the Classic
   scheme is clearly incorrect over the white background of the Light scheme.
   A selection of a preset is a decision about the full appearance, and not about
   two of the three colors.
   Thus a preset discards a shadow color that a user set manually in
   Advanced Colors.... A preset already does the same operation for a manual
   pixel color or background color.
   This code calculates the shadow color around the manual ink color of each
   preset. It does not send the background of the preset through
   derive_theme_colors. The color pairs of the presets are deliberate, and they
   stay exactly as a person wrote them. */
static void apply_display_colors(menu_context_t *ctx, uint32_t pixel_rgba, uint32_t bg_rgba) {
    apply_display_colors_full(ctx, pixel_rgba, bg_rgba, theme_shadow_for(bg_rgba, pixel_rgba));
}

/* Writes the result of ChooseColorA into the hex edit control at `edit_id`. It
   also reports whether the user selected a color.
   This function gives ChooseColorA the current text of that field as its start
   value. If that text is not valid hex, it uses black.
   A "Choose..." selection and a manual entry of the hex code can both occur:
   each one writes over the same field.
   The return value is important for IDC_SCREEN_CHOOSE, which calculates the
   other two colors after this call. A calculation after a cancelled selection
   discards manual colors and gives nothing in exchange. */
static int choose_color_into_hex_field(HWND hdlg, int edit_id) {
    /* CHOOSECOLOR needs an array of 16 custom colors that the caller owns.
       This array is static. Thus the custom colors of the user continue between
       selections, in one dialog session and across separate menu selections. The
       array does not return to its initial state at each call. */
    static COLORREF custom_colors[16] = {0};
    char text[7], new_hex[7];
    uint8_t r, g, b;
    CHOOSECOLORA cc;

    GetDlgItemTextA(hdlg, edit_id, text, sizeof(text));
    memset(&cc, 0, sizeof(cc));
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = hdlg;
    cc.lpCustColors = custom_colors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    cc.rgbResult = parse_hex_rgb(text, &r, &g, &b) ? RGB(r, g, b) : RGB(0, 0, 0);
    if (!ChooseColorA(&cc)) {
        return 0;
    }
    snprintf(new_hex, sizeof(new_hex), "%02X%02X%02X", GetRValue(cc.rgbResult), GetGValue(cc.rgbResult),
        GetBValue(cc.rgbResult));
    SetDlgItemTextA(hdlg, edit_id, new_hex);
    return 1;
}

/* The lParam data for advanced_colors_dialog_proc.
   The pattern is the same as the pattern of hwid_dialog_data_t: this code writes
   the parsed_*_rgba fields, and permits IDOK to close the dialog, only after
   parse_hex_rgb accepts each hex field.
   At OK, the three hex fields are always the authority. The calculation is never
   the authority, because a manual color must continue. IDC_SCREEN_CHOOSE and
   IDC_REMATCH are the only controls that write a field that the user did not
   select. Both of those controls are clear requests for a new calculation. */
typedef struct {
    char pixel_hex[7];
    char bg_hex[7];
    char shadow_hex[7];
    /* This field holds the state of IDC_SHADOWS_ENABLE. Thus the preview draws
       the ghosting trail only while that control is on. It does not show a trail
       that is not in use. This app applies this field at OK, the same as each
       other field here. */
    int show_shadows;
    int custom_visible;
    /* The window heights in pixels. This code measures expanded_height from the
       template at WM_INITDIALOG. It calculates collapsed_height from the
       position of IDC_CUSTOM_TOGGLE. Thus neither value is a second copy of a
       number in resource.rc. */
    int collapsed_height;
    int expanded_height;
    uint32_t parsed_pixel_rgba;
    uint32_t parsed_bg_rgba;
    uint32_t parsed_shadow_rgba;
} advanced_colors_dialog_data_t;

/* Each control in the Custom Colors group box, and the group box itself. A
   hidden control leaves the tab order automatically. Thus a user cannot move the
   keyboard focus into the fields that a collapsed dialog hides. */
static const int advanced_colors_custom_ids[] = {IDC_CUSTOM_GROUP, IDC_CUSTOM_BG_LABEL, IDC_BG_HEX, IDC_BG_CHOOSE,
    IDC_CUSTOM_PIXEL_LABEL, IDC_PIXEL_HEX, IDC_PIXEL_CHOOSE, IDC_CUSTOM_SHADOW_LABEL, IDC_SHADOW_HEX,
    IDC_SHADOW_CHOOSE, IDC_REMATCH};

/* An 8x8 sample sprite for the preview area. The MSB (0x80) is the leftmost
   column.
   The sprite is a face, and not a part of a real PocketStation frame. It has
   single lit pixels, continuous groups of lit pixels, and a flat bottom edge.
   Thus it shows each part of the scheme: the ink, the background, and the
   ghosting row below a lit pixel. */
static const uint8_t preview_sprite[8] = {0x3C, 0x42, 0xA5, 0x81, 0xA5, 0x99, 0x42, 0x3C};

static COLORREF rgba_to_colorref(uint32_t rgba) {
    return RGB((rgba >> 24) & 0xFFu, (rgba >> 16) & 0xFFu, (rgba >> 8) & 0xFFu);
}

/* Reads `edit_id`'s current text as a color, falling back to `fallback` while
   the field is mid-edit and does not parse yet. */
static uint32_t hex_field_rgba(HWND hdlg, int edit_id, uint32_t fallback) {
    char text[7];
    uint8_t r, g, b;
    GetDlgItemTextA(hdlg, edit_id, text, sizeof(text));
    return parse_hex_rgb(text, &r, &g, &b) ? RGBA_PACK(r, g, b) : fallback;
}

/* Draws the IDC_COLOR_PREVIEW area. It renders preview_sprite exactly the way
   that render_framebuffer renders the real LCD, in the colors that the three hex
   fields hold. This preview answers the question "what do these three colors look
   like together". A usual color selection dialog does not answer that
   question. */
static void draw_color_preview(HWND hdlg, const DRAWITEMSTRUCT *dis) {
    advanced_colors_dialog_data_t *data = (advanced_colors_dialog_data_t *)GetWindowLongPtrA(hdlg, GWLP_USERDATA);
    uint32_t bg_rgba = hex_field_rgba(hdlg, IDC_BG_HEX, DISPLAY_BG_CLASSIC);
    uint32_t pixel_rgba = hex_field_rgba(hdlg, IDC_PIXEL_HEX, DISPLAY_PIXEL_CLASSIC);
    uint32_t shadow_rgba = hex_field_rgba(hdlg, IDC_SHADOW_HEX, DISPLAY_SHADOW_COLOR);
    RECT rc = dis->rcItem;
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    /* 8 sprite rows, and the ghosting row below them. The remainder of the height
       is a margin. Thus the background looks like a screen. */
    int cell = height / 12;
    int origin_x, origin_y, row, col;
    HBRUSH bg_brush, pixel_brush, shadow_brush;
    int show_shadows = data ? data->show_shadows : 0;

    if (cell < 1) {
        cell = 1;
    }
    origin_x = rc.left + (width - cell * 8) / 2;
    origin_y = rc.top + (height - cell * 9) / 2;

    bg_brush = CreateSolidBrush(rgba_to_colorref(bg_rgba));
    pixel_brush = CreateSolidBrush(rgba_to_colorref(pixel_rgba));
    shadow_brush = CreateSolidBrush(rgba_to_colorref(shadow_rgba));
    FillRect(dis->hDC, &rc, bg_brush);
    for (row = 0; row < 8; row++) {
        for (col = 0; col < 8; col++) {
            RECT cell_rc;
            int lit = (preview_sprite[row] >> (7 - col)) & 1;
            int lit_below = row < 7 ? (preview_sprite[row + 1] >> (7 - col)) & 1 : 0;
            int draw_row;
            if (!lit) {
                continue;
            }
            /* The same two conditions as the second pass of render_framebuffer:
               the ghost is one row below a lit pixel, and only where that row is
               dark. Thus two lit pixels in a vertical line never make each other
               darker. */
            for (draw_row = 0; draw_row < 2; draw_row++) {
                if (draw_row == 1 && (lit_below || !show_shadows)) {
                    continue;
                }
                cell_rc.left = origin_x + col * cell;
                cell_rc.top = origin_y + (row + draw_row) * cell;
                cell_rc.right = cell_rc.left + cell;
                cell_rc.bottom = cell_rc.top + cell;
                FillRect(dis->hDC, &cell_rc, draw_row == 0 ? pixel_brush : shadow_brush);
            }
        }
    }
    DeleteObject(bg_brush);
    DeleteObject(pixel_brush);
    DeleteObject(shadow_brush);
}

/* Writes the active pixel field and the sprite shadow field again, from the
   current value of IDC_BG_HEX. Only IDC_SCREEN_CHOOSE and IDC_REMATCH call this
   function. Thus this code never writes over a manual color without a request
   from the user.
   This function does not change a field that it cannot parse, and it does not
   report an error. An incomplete hex code gives no data for a calculation, and
   the user is in the middle of an edit. The user did not make an error. */
static void rematch_derived_fields(HWND hdlg) {
    char text[7], hex[7];
    uint8_t r, g, b;
    uint32_t pixel_rgba, shadow_rgba;
    GetDlgItemTextA(hdlg, IDC_BG_HEX, text, sizeof(text));
    if (!parse_hex_rgb(text, &r, &g, &b)) {
        return;
    }
    derive_theme_colors(RGBA_PACK(r, g, b), &pixel_rgba, &shadow_rgba);
    format_rgba_hex(pixel_rgba, hex, sizeof(hex));
    SetDlgItemTextA(hdlg, IDC_PIXEL_HEX, hex);
    format_rgba_hex(shadow_rgba, hex, sizeof(hex));
    SetDlgItemTextA(hdlg, IDC_SHADOW_HEX, hex);
}

static void set_custom_colors_visible(HWND hdlg, advanced_colors_dialog_data_t *data, int visible) {
    RECT window_rect;
    size_t i;
    for (i = 0; i < sizeof(advanced_colors_custom_ids) / sizeof(advanced_colors_custom_ids[0]); i++) {
        ShowWindow(GetDlgItem(hdlg, advanced_colors_custom_ids[i]), visible ? SW_SHOW : SW_HIDE);
    }
    SetDlgItemTextA(hdlg, IDC_CUSTOM_TOGGLE, visible ? "Custom Colors <<" : "Custom Colors >>");
    GetWindowRect(hdlg, &window_rect);
    SetWindowPos(hdlg, NULL, 0, 0, window_rect.right - window_rect.left,
        visible ? data->expanded_height : data->collapsed_height, SWP_NOMOVE | SWP_NOZORDER);
    data->custom_visible = visible;
}

static INT_PTR CALLBACK advanced_colors_dialog_proc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_INITDIALOG: {
        advanced_colors_dialog_data_t *data = (advanced_colors_dialog_data_t *)lparam;
        RECT window_rect, client, toggle, margin = {0, 0, 0, 7};
        SetWindowLongPtrA(hdlg, GWLP_USERDATA, (LONG_PTR)data);

        /* Measure the size before this code hides or resizes a control. A person
           wrote the template at its expanded size (see resource.rc). The
           collapsed size ends one dialog-unit margin below the Custom Colors
           toggle row. This code reads that position from the live control. It
           does not use a second copy of the number here. */
        GetWindowRect(hdlg, &window_rect);
        data->expanded_height = window_rect.bottom - window_rect.top;
        GetClientRect(hdlg, &client);
        GetWindowRect(GetDlgItem(hdlg, IDC_CUSTOM_TOGGLE), &toggle);
        MapWindowPoints(NULL, hdlg, (POINT *)&toggle, 2);
        MapDialogRect(hdlg, &margin);
        data->collapsed_height =
            (data->expanded_height - (client.bottom - client.top)) + toggle.bottom + margin.bottom;

        SetDlgItemTextA(hdlg, IDC_BG_HEX, data->bg_hex);
        SetDlgItemTextA(hdlg, IDC_PIXEL_HEX, data->pixel_hex);
        SetDlgItemTextA(hdlg, IDC_SHADOW_HEX, data->shadow_hex);
        SendDlgItemMessageA(hdlg, IDC_BG_HEX, EM_SETLIMITTEXT, 6, 0);
        SendDlgItemMessageA(hdlg, IDC_PIXEL_HEX, EM_SETLIMITTEXT, 6, 0);
        SendDlgItemMessageA(hdlg, IDC_SHADOW_HEX, EM_SETLIMITTEXT, 6, 0);
        CheckDlgButton(hdlg, IDC_SHADOWS_ENABLE, data->show_shadows ? BST_CHECKED : BST_UNCHECKED);
        /* This dialog always opens in its collapsed form. The purpose of the
           calculation of the other two colors is one decision for the usual
           condition, and not three decisions. */
        set_custom_colors_visible(hdlg, data, 0);
        return TRUE;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lparam;
        if (dis->CtlID == IDC_COLOR_PREVIEW) {
            draw_color_preview(hdlg, dis);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        advanced_colors_dialog_data_t *data = (advanced_colors_dialog_data_t *)GetWindowLongPtrA(hdlg, GWLP_USERDATA);
        if (HIWORD(wparam) == EN_CHANGE) {
            int edit_id = LOWORD(wparam);
            if (edit_id != IDC_BG_HEX && edit_id != IDC_PIXEL_HEX && edit_id != IDC_SHADOW_HEX) {
                break;
            }
            /* A manual entry of a color only draws the preview again. A manual
               edit of the screen color deliberately does *not* calculate the
               other two colors again. In the Custom Colors group, all three
               fields belong to the user, and IDC_REMATCH calculates them again
               when the user asks. */
            InvalidateRect(GetDlgItem(hdlg, IDC_COLOR_PREVIEW), NULL, TRUE);
            return TRUE;
        }
        switch (LOWORD(wparam)) {
        case IDC_SCREEN_CHOOSE:
            /* The one-color path: the user selects a screen color, and this code
               calculates the applicable pixel color and shadow color. A
               cancelled selection causes no change. Thus this code can never
               discard manual colors without a message. */
            if (choose_color_into_hex_field(hdlg, IDC_BG_HEX)) {
                rematch_derived_fields(hdlg);
            }
            return TRUE;
        case IDC_BG_CHOOSE:
            /* Each of these controls writes its own field with SetDlgItemTextA.
               Thus the EN_CHANGE path above draws the preview again. */
            choose_color_into_hex_field(hdlg, IDC_BG_HEX);
            return TRUE;
        case IDC_PIXEL_CHOOSE:
            choose_color_into_hex_field(hdlg, IDC_PIXEL_HEX);
            return TRUE;
        case IDC_SHADOW_CHOOSE:
            choose_color_into_hex_field(hdlg, IDC_SHADOW_HEX);
            return TRUE;
        case IDC_REMATCH:
            rematch_derived_fields(hdlg);
            return TRUE;
        case IDC_SHADOWS_ENABLE:
            /* AUTOCHECKBOX already changed its own state before this message
               arrives. This code copies that state, thus the preview agrees with
               the control. */
            data->show_shadows = IsDlgButtonChecked(hdlg, IDC_SHADOWS_ENABLE) == BST_CHECKED;
            InvalidateRect(GetDlgItem(hdlg, IDC_COLOR_PREVIEW), NULL, TRUE);
            return TRUE;
        case IDC_CUSTOM_TOGGLE:
            set_custom_colors_visible(hdlg, data, !data->custom_visible);
            return TRUE;
        case IDOK: {
            char pixel_text[7], bg_text[7], shadow_text[7];
            uint8_t px_r, px_g, px_b, bg_r, bg_g, bg_b, sh_r, sh_g, sh_b;
            GetDlgItemTextA(hdlg, IDC_PIXEL_HEX, pixel_text, sizeof(pixel_text));
            GetDlgItemTextA(hdlg, IDC_BG_HEX, bg_text, sizeof(bg_text));
            GetDlgItemTextA(hdlg, IDC_SHADOW_HEX, shadow_text, sizeof(shadow_text));
            if (!parse_hex_rgb(pixel_text, &px_r, &px_g, &px_b) || !parse_hex_rgb(bg_text, &bg_r, &bg_g, &bg_b)
                || !parse_hex_rgb(shadow_text, &sh_r, &sh_g, &sh_b)) {
                MessageBoxA(hdlg, "Every color needs exactly 6 hex digits (0-9, A-F), e.g. \"1A2B3C\".",
                    "pokketstation", MB_ICONERROR);
                return TRUE;
            }
            data->parsed_pixel_rgba = RGBA_PACK(px_r, px_g, px_b);
            data->parsed_bg_rgba = RGBA_PACK(bg_r, bg_g, bg_b);
            data->parsed_shadow_rgba = RGBA_PACK(sh_r, sh_g, sh_b);
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

static void prompt_advanced_colors(menu_context_t *ctx) {
    advanced_colors_dialog_data_t data;
    memset(&data, 0, sizeof(data));
    format_rgba_hex(*ctx->pixel_rgba, data.pixel_hex, sizeof(data.pixel_hex));
    format_rgba_hex(*ctx->bg_rgba, data.bg_hex, sizeof(data.bg_hex));
    format_rgba_hex(*ctx->shadow_rgba, data.shadow_hex, sizeof(data.shadow_hex));
    data.show_shadows = *ctx->show_shadows;
    if (DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_ADVANCED_COLORS), ctx->hwnd,
            advanced_colors_dialog_proc, (LPARAM)&data) != IDOK) {
        return;
    }
    /* This code sets the sprite shadow control here, and not through a separate
       function. Thus all four values of this dialog go into one save_settings
       call. This app does not write settings.cfg two times for one OK
       selection. */
    *ctx->show_shadows = data.show_shadows;
    ctx->settings->show_shadows = data.show_shadows;
    apply_display_colors_full(ctx, data.parsed_pixel_rgba, data.parsed_bg_rgba, data.parsed_shadow_rgba);
}

/* IDD_CAPTURE_PROMPT shows only static text.
   It has no buttons. Thus this handler permits only the default dialog
   operations. */
static INT_PTR CALLBACK capture_prompt_dialog_proc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    (void)hdlg;
    (void)wparam;
    (void)lparam;
    return msg == WM_INITDIALOG ? TRUE : FALSE;
}

/* This function blocks the caller. It reads the SDL event queue directly.
   DialogBoxParamA blocks the caller in the same way, with its own message loop.
   This function returns the key that the user presses.
   It returns SDL_SCANCODE_UNKNOWN if the user presses Esc or closes the window.
   The caller treats that value as a cancel.

   This function uses SDL_PollEvent. It does not use a platform dialog or a
   platform message loop. To capture a raw Win32 WM_KEYDOWN message and convert
   it to an SDL_Scancode manually needs a second copy of the per-platform
   scancode table of SDL. That table is complex. button_scancodes[].scancode and
   SDL_GetKeyboardState already depend on SDL for that conversion.
   A request to SDL is safer, and it is the same method that the usual input path
   uses.

   A real SDL_QUIT event during this wait cannot return correctly through the
   call stack that reaches this point. That stack is the WM_COMMAND handler, and
   then this function. Thus this function also treats SDL_QUIT as a cancel.
   The next close request from the user then operates normally, because this
   function blocks only for one key press. */
static SDL_Scancode capture_next_key(HWND hwnd, const char *button_name) {
    char message[160];
    HWND prompt;
    RECT owner_rect, prompt_rect;
    int have_result = 0;
    SDL_Scancode result = SDL_SCANCODE_UNKNOWN;

    /* This code uses CreateDialogParamA, which is modeless. It does not use
       DialogBoxParamA. A modal dialog prevents this function from reaching the
       SDL_PollEvent loop below. MessageBoxA had the same problem.
       This code shows the dialog with SW_SHOWNOACTIVATE. Thus the dialog never
       takes the activation or the keyboard focus from `hwnd`.
       Without SW_SHOWNOACTIVATE, the menu-mnemonic function of Windows consumed
       the key press, which the user heard as the system beep. The key press then
       never got to SDL as a real SDL_KEYDOWN event. */
    prompt = CreateDialogParamA(
        GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_CAPTURE_PROMPT), hwnd, capture_prompt_dialog_proc, 0);
    snprintf(message, sizeof(message), "Press the key you want to use for %s.\n\nPress Esc to cancel.", button_name);
    SetDlgItemTextA(prompt, IDC_CAPTURE_PROMPT_TEXT, message);
    GetWindowRect(hwnd, &owner_rect);
    GetWindowRect(prompt, &prompt_rect);
    SetWindowPos(prompt, HWND_TOP,
        owner_rect.left + ((owner_rect.right - owner_rect.left) - (prompt_rect.right - prompt_rect.left)) / 2,
        owner_rect.top + ((owner_rect.bottom - owner_rect.top) - (prompt_rect.bottom - prompt_rect.top)) / 2, 0, 0,
        SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(prompt, SW_SHOWNOACTIVATE);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    while (!have_result) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_KEYDOWN) {
                result = event.key.keysym.scancode == SDL_SCANCODE_ESCAPE ? SDL_SCANCODE_UNKNOWN
                                                                           : event.key.keysym.scancode;
                have_result = 1;
                break;
            }
            if (event.type == SDL_QUIT) {
                have_result = 1;
                break;
            }
        }
        if (!have_result) {
            SDL_Delay(10);
        }
    }
    DestroyWindow(prompt);
    return result;
}

static INT_PTR CALLBACK remap_dialog_proc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_INITDIALOG: {
        button_binding_t *bindings = (button_binding_t *)lparam;
        int i;
        SetWindowLongPtrA(hdlg, GWLP_USERDATA, (LONG_PTR)bindings);
        for (i = 0; i < REMAP_BINDING_COUNT; i++) {
            /* SDL_GetScancodeName(SDL_SCANCODE_UNKNOWN) returns "".
               Show a label that the user recognizes as "a different row took the
               key of this row" (see prompt_remap_controls). Do not show an empty
               label. */
            const char *name =
                bindings[i].scancode == SDL_SCANCODE_UNKNOWN ? "(unbound)" : SDL_GetScancodeName(bindings[i].scancode);
            SetDlgItemTextA(hdlg, IDC_REMAP_LABEL_BASE + i, name);
        }
        return TRUE;
    }
    case WM_COMMAND: {
        int cmd = LOWORD(wparam);
        if (cmd == IDOK || cmd == IDCANCEL) {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        if (cmd >= IDC_REMAP_CHANGE_BASE && cmd < IDC_REMAP_CHANGE_BASE + REMAP_BINDING_COUNT) {
            /* Values of 100 and above are outside each real IDOK, IDCANCEL, and
               control ID. prompt_remap_controls uses this range to tell "the user
               selected Change... in row N" from a simple close. */
            EndDialog(hdlg, 100 + (cmd - IDC_REMAP_CHANGE_BASE));
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

/* Shows the current REMAP_BINDING_COUNT key bindings. Each row has its own
   "Change..." button.
   A selection of a "Change..." button closes this dialog (see
   remap_dialog_proc). Thus capture_next_key below can capture the key press,
   outside of the keyboard-navigation message loop of a platform dialog.
   This function then opens the dialog again, to show the result and to permit a
   change to a different row.
   This loop continues until the user selects Close. */
static void prompt_remap_controls(menu_context_t *ctx) {
    for (;;) {
        INT_PTR result = DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_REMAP_CONTROLS), ctx->hwnd,
            remap_dialog_proc, (LPARAM)ctx->button_scancodes);
        int index;
        SDL_Scancode captured;
        if (result < 100) {
            break;
        }
        index = (int)(result - 100);
        captured = capture_next_key(ctx->hwnd, ctx->button_scancodes[index].display_name);
        if (captured != SDL_SCANCODE_UNKNOWN) {
            int i;
            /* Two rows cannot share a key.
               Whichever other row previously held this key becomes unbound.
               Otherwise, both rows would silently point at the same physical key.
               SDL_GetKeyboardState could then never tell the two rows apart. */
            for (i = 0; i < REMAP_BINDING_COUNT; i++) {
                if (i != index && ctx->button_scancodes[i].scancode == captured) {
                    ctx->button_scancodes[i].scancode = SDL_SCANCODE_UNKNOWN;
                }
            }
            ctx->button_scancodes[index].scancode = captured;
            save_key_bindings(ctx->settings, ctx->button_scancodes);
            save_settings(ctx->settings, ctx->settings_path);
        }
    }
}

static INT_PTR CALLBACK about_dialog_proc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextA(hdlg, IDC_ABOUT_VERSION, "pokketstation " POKKETSTATION_VERSION);
        return TRUE;
    case WM_NOTIFY: {
        NMHDR *nmhdr = (NMHDR *)lparam;
        if (nmhdr->idFrom == IDC_ABOUT_LINK && (nmhdr->code == NM_CLICK || nmhdr->code == NM_RETURN)) {
            ShellExecuteA(hdlg, "open", "https://github.com/mentalfoundry/pokketstation", NULL, NULL, SW_SHOWNORMAL);
            return TRUE;
        }
        if (nmhdr->idFrom == IDC_ABOUT_LICENSE_LINK && (nmhdr->code == NM_CLICK || nmhdr->code == NM_RETURN)) {
            ShellExecuteA(hdlg, "open", "https://www.gnu.org/licenses/gpl-3.0.html", NULL, NULL, SW_SHOWNORMAL);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) {
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void show_about(menu_context_t *ctx) {
    DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_ABOUT), ctx->hwnd, about_dialog_proc, 0);
}

/* The handlers for IR Link > Host Session, Connect, and Disconnect.
   These handlers use one fixed pipe name, IR_LINK_DEFAULT_PIPE_NAME. They do not use a dialog that asks
   for a name. One fixed name is the simplest method that operates for two instances on one machine.
   See ir_link.h for the transport. */
static void ir_link_host_from_menu(menu_context_t *ctx) {
    ir_link_host(ctx->ir_link, IR_LINK_DEFAULT_PIPE_NAME);
    ir_link_refresh_title(ctx);
}

static void ir_link_connect_from_menu(menu_context_t *ctx) {
    ir_link_connect(ctx->ir_link, IR_LINK_DEFAULT_PIPE_NAME);
    ir_link_refresh_title(ctx);
}

static void ir_link_disconnect_from_menu(menu_context_t *ctx) {
    ir_link_disconnect(ctx->ir_link);
    ir_link_refresh_title(ctx);
}

/* Gives the color preset that is in effect, as its menu command ID. It returns 0
   if no preset is in effect.

   This app stores no "current scheme" value. A scheme is only the three colors
   that it produced, and Advanced Colors... can set those colors to any values.
   Thus this function finds the active preset with a different method: it asks
   which preset, at this time, would produce exactly the colors that are in
   effect. The shadow color is part of that comparison, because a preset sets all
   three colors (see apply_display_colors). A manual shadow color over the ink and
   the background of the Classic scheme is no longer the Classic scheme, and this
   app must not report it as the Classic scheme.

   A return value of 0 for a custom scheme is deliberate: the menu then shows no
   checked preset, which is correct. It does not check the preset that supplied
   the last calculation. */
static int active_color_preset_id(const menu_context_t *ctx) {
    static const struct {
        int id;
        uint32_t pixel;
        uint32_t bg;
    } presets[] = {
        {ID_COLORS_CLASSIC, DISPLAY_PIXEL_CLASSIC, DISPLAY_BG_CLASSIC},
        {ID_COLORS_STANDARD, DISPLAY_PIXEL_LIGHT, DISPLAY_BG_LIGHT},
        {ID_COLORS_REVERSED, DISPLAY_PIXEL_DARK, DISPLAY_BG_DARK},
    };
    for (int i = 0; i < (int)(sizeof(presets) / sizeof(presets[0])); i++) {
        if (*ctx->pixel_rgba == presets[i].pixel && *ctx->bg_rgba == presets[i].bg &&
            *ctx->shadow_rgba == theme_shadow_for(presets[i].bg, presets[i].pixel)) {
            return presets[i].id;
        }
    }
    return 0;
}

/* CheckMenuRadioItem needs the menu that contains the group directly. Both
   override groups and the Colors group are in nested popup menus. Thus this code
   finds the containing popup menu by its command ID. It does not use fixed
   submenu positions, which become incorrect after a change to the menu order. */
static HMENU find_menu_containing(HMENU menu, UINT id) {
    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; i++) {
        HMENU sub = GetSubMenu(menu, i);
        if (sub != NULL) {
            HMENU found = find_menu_containing(sub, id);
            if (found != NULL) {
                return found;
            }
        } else if (GetMenuItemID(menu, i) == id) {
            return menu;
        }
    }
    return NULL;
}

/* Puts each checkable menu item in agreement with the current state.

   This app calls this function after each menu command. It does not call the
   function from WM_INITMENUPOPUP, which is the usual location. SDL2 calls its
   Windows message hook from inside its own PeekMessage loop, thus the hook sees
   only POSTED messages. The menu posts its WM_COMMAND message, which is why the
   command handlers operate. The menu system sends WM_INITMENUPOPUP directly to
   the window procedure, and that message never enters the queue. Thus a handler
   for it there never executes. To receive that message, this app must subclass
   the window.

   A call after each command is sufficient, because only the commands below
   change this state. The menu sets the overrides. The menu, or a dialog that one
   of those commands opens, sets the color scheme. The loaded BIOS controls
   whether the override groups are usable, and only a menu command changes the
   loaded BIOS. */
static void sync_menu_state(menu_context_t *ctx) {
    HMENU root = GetMenu(ctx->hwnd);
    if (root == NULL) {
        return;
    }

    /* The override addresses are valid only on a BIOS revision that this project
       traced. On each other revision, this code disables the group. It does not
       leave the group available (see psemu_settings_offsets_known). */
    int usable = psemu_settings_offsets_known(ctx->ps);
    static const int datetime_ids[] = {ID_TOOLS_DATETIME_DEFAULT, ID_TOOLS_DATETIME_OS};

    HMENU datetime_menu = find_menu_containing(root, ID_TOOLS_DATETIME_DEFAULT);
    if (datetime_menu != NULL) {
        for (int i = 0; i < (int)(sizeof(datetime_ids) / sizeof(datetime_ids[0])); i++) {
            EnableMenuItem(datetime_menu, datetime_ids[i], MF_BYCOMMAND | (usable ? MF_ENABLED : MF_GRAYED));
        }
        CheckMenuRadioItem(
            datetime_menu, ID_TOOLS_DATETIME_DEFAULT, ID_TOOLS_DATETIME_OS,
            datetime_ids[ctx->settings->datetime_override], MF_BYCOMMAND);
    }

    /* Tools > Sound > Volume is the output level of this app. Thus this code
       never disables it, and it operates with no BIOS. A stored percentage that
       is not a step of 10% rounds to the nearest item that the menu can show.
       Only a manual edit of settings.cfg can give such a value. This code does
       not change the stored value. The menu cannot draw it. */
    HMENU volume_menu = find_menu_containing(root, ID_TOOLS_SOUND_BASE);
    if (volume_menu != NULL) {
        int step = (clamp_master_volume(ctx->settings->master_volume) + 5) / 10;
        CheckMenuRadioItem(
            volume_menu, ID_TOOLS_SOUND_BASE, ID_TOOLS_SOUND_LAST, ID_TOOLS_SOUND_BASE + step, MF_BYCOMMAND);
    }

    /* Tools > Sound > Speaker is next to Volume, and it is not inside Volume.
       Thus it needs its own search: find_menu_containing returns the popup menu
       that holds the group directly, and these are two different popup menus.
       This code never disables this group, for the same reason as Volume: it is
       the output filter of this app, and it needs no BIOS. */
    HMENU speaker_menu = find_menu_containing(root, ID_TOOLS_SPEAKER_BASE);
    if (speaker_menu != NULL) {
        CheckMenuRadioItem(speaker_menu, ID_TOOLS_SPEAKER_BASE, ID_TOOLS_SPEAKER_LAST,
            ID_TOOLS_SPEAKER_BASE + clamp_speaker_sim(ctx->settings->speaker_sim), MF_BYCOMMAND);
    }

    /* The radio range ends at ID_COLORS_CLASSIC. Thus "Advanced Colors..." stays
       an action item, and it does not join the group. That item opens a dialog.
       It is not a scheme that can be active. A custom scheme checks no item, thus
       this code clears each preset first. It does not leave the last checked
       preset. */
    HMENU colors_menu = find_menu_containing(root, ID_COLORS_CLASSIC);
    if (colors_menu != NULL) {
        static const int color_ids[] = {ID_COLORS_CLASSIC, ID_COLORS_STANDARD, ID_COLORS_REVERSED};
        for (int i = 0; i < (int)(sizeof(color_ids) / sizeof(color_ids[0])); i++) {
            CheckMenuItem(colors_menu, color_ids[i], MF_BYCOMMAND | MF_UNCHECKED);
        }
        int preset = active_color_preset_id(ctx);
        if (preset != 0) {
            CheckMenuRadioItem(colors_menu, ID_COLORS_STANDARD, ID_COLORS_CLASSIC, preset, MF_BYCOMMAND);
        }
    }
}

/* SDL_SetWindowsMessageHook installs this function.
   It executes synchronously from the message loop of SDL_PollEvent, on the same
   thread. Thus no lock is necessary.
   This function can change *ctx and call psemu_* functions directly. */
static void SDLCALL handle_windows_message(void *userdata, void *hwnd, unsigned int message, Uint64 wparam,
    Sint64 lparam) {
    (void)hwnd;
    (void)lparam;
    menu_context_t *ctx = (menu_context_t *)userdata;
    if (message != WM_COMMAND) {
        return;
    }
    /* Tools > Sound > Volume uses one contiguous ID range. It does not use a
       separate named ID for each step (see resource.h). Thus this code tests it
       here, and not as eleven cases in the selection below. The main loop reads
       settings->master_volume at each frame, thus a store of the value applies
       it. */
    if (LOWORD(wparam) >= ID_TOOLS_SOUND_BASE && LOWORD(wparam) <= ID_TOOLS_SOUND_LAST) {
        ctx->settings->master_volume = (int)(LOWORD(wparam) - ID_TOOLS_SOUND_BASE) * 10;
        save_settings(ctx->settings, ctx->settings_path);
        sync_menu_state(ctx);
        return;
    }
    /* Tools > Sound > Speaker uses the same method, for the same reason.
       A store of the value also applies it: the main loop reads
       settings->speaker_sim at each frame, and the filter calculates its
       coefficients again at a change of the value (see apply_speaker_filter). */
    if (LOWORD(wparam) >= ID_TOOLS_SPEAKER_BASE && LOWORD(wparam) <= ID_TOOLS_SPEAKER_LAST) {
        ctx->settings->speaker_sim = (int)(LOWORD(wparam) - ID_TOOLS_SPEAKER_BASE);
        save_settings(ctx->settings, ctx->settings_path);
        sync_menu_state(ctx);
        return;
    }
    /* File > Save State and File > Load State each use one contiguous ID range
       (see resource.h). This code tests them the same way as the two Sound
       groups above.
       Slot 0 of each group is the quick slot. Thus these two tests also cover the
       operation of the Save State and Load State hotkeys. */
    if (LOWORD(wparam) >= ID_FILE_SAVE_SLOT_BASE && LOWORD(wparam) <= ID_FILE_SAVE_SLOT_LAST) {
        save_state_to_slot(ctx, (int)(LOWORD(wparam) - ID_FILE_SAVE_SLOT_BASE));
        return;
    }
    if (LOWORD(wparam) >= ID_FILE_LOAD_SLOT_BASE && LOWORD(wparam) <= ID_FILE_LOAD_SLOT_LAST) {
        load_state_from_slot(ctx, (int)(LOWORD(wparam) - ID_FILE_LOAD_SLOT_BASE));
        return;
    }
    switch (LOWORD(wparam)) {
    case ID_FILE_OPEN_BIOS:
        prompt_open_bios(ctx);
        break;
    case ID_FILE_OPEN_APP:
        prompt_open_app(ctx);
        break;
    case ID_FILE_RESET:
        reset_emulation(ctx);
        break;
    case ID_FILE_EXIT:
        *ctx->running = 0;
        break;
    case ID_TOOLS_EDIT_HWID:
        prompt_edit_hardware_id(ctx);
        break;
    case ID_TOOLS_REMAP_CONTROLS:
        prompt_remap_controls(ctx);
        break;
    /* A change to Default only stops the frontend from a write of its value at
       each frame. The last value of the BIOS stays in position, and the system
       menus of the BIOS then operate normally. */
    case ID_TOOLS_DATETIME_DEFAULT:
    case ID_TOOLS_DATETIME_OS:
        ctx->settings->datetime_override = (int)(LOWORD(wparam) - ID_TOOLS_DATETIME_DEFAULT);
        save_settings(ctx->settings, ctx->settings_path);
        break;
    case ID_VIEW_NATIVE_SIZE:
        resize_client_to_scale(ctx->hwnd, 1);
        break;
    case ID_VIEW_DOUBLE_SIZE:
        resize_client_to_scale(ctx->hwnd, 2);
        break;
    case ID_COLORS_STANDARD:
        apply_display_colors(ctx, DISPLAY_PIXEL_LIGHT, DISPLAY_BG_LIGHT);
        break;
    case ID_COLORS_REVERSED:
        apply_display_colors(ctx, DISPLAY_PIXEL_DARK, DISPLAY_BG_DARK);
        break;
    case ID_COLORS_CLASSIC:
        apply_display_colors(ctx, DISPLAY_PIXEL_CLASSIC, DISPLAY_BG_CLASSIC);
        break;
    case ID_COLORS_ADVANCED:
        prompt_advanced_colors(ctx);
        break;
    case ID_HELP_ABOUT:
        show_about(ctx);
        break;
    case ID_IR_HOST:
        ir_link_host_from_menu(ctx);
        break;
    case ID_IR_CONNECT:
        ir_link_connect_from_menu(ctx);
        break;
    case ID_IR_DISCONNECT:
        ir_link_disconnect_from_menu(ctx);
        break;
    }
    /* Each command that can change the state of a checkable item uses the
       selection above. Thus one call here covers all of them. This includes a
       color scheme change in the Advanced Colors... dialog, and the override
       groups that become usable after a BIOS loads. See sync_menu_state for the
       reason that this app does not do this from WM_INITMENUPOPUP. */
    sync_menu_state(ctx);
}

static int lcd_bit_on(const uint8_t *fb, int row, int col) {
    int byte_index = row * PSEMU_LCD_STRIDE + col / 8;
    int bit_index = col % 8;
    return (fb[byte_index] >> bit_index) & 1;
}

static void render_framebuffer(const psemu_t *ps, uint32_t *pixels, uint32_t pixel_rgba, uint32_t bg_rgba,
    int show_shadows, uint32_t shadow_rgba) {
    const uint8_t *fb = psemu_get_framebuffer(ps);
    for (int row = 0; row < PSEMU_LCD_HEIGHT; row++) {
        for (int col = 0; col < PSEMU_LCD_WIDTH; col++) {
            /* SDL_PIXELFORMAT_RGBA8888 holds a 32-bit value as
               (R<<24)|(G<<16)|(B<<8)|A, for each host byte order.
               For example, 0xFF000000 means R = 0xFF, G = 0, B = 0, and A = 0.
               That value is pure red with a transparent alpha value. It is not
               opaque black.
               pixel_rgba and bg_rgba use this layout (see RGBA_PACK).
               A caller must not supply a 0xRRGGBB value here. */
            pixels[row * PSEMU_LCD_WIDTH + col] = lcd_bit_on(fb, row, col) ? pixel_rgba : bg_rgba;
        }
    }
    if (!show_shadows) {
        return;
    }
    /* An approximation of the small "ghosting" that a real STN passive-matrix LCD
       from the late 1990s shows after a lit pixel.
       The cause of this ghosting is the slow response of the liquid crystal. It is
       not a drop shadow.
       This code draws shadow_rgba one row below each lit pixel. That color agrees
       with the active color scheme, and a user can set it manually from
       View > Colors > Advanced Colors....
       This code is a second pass. Thus it never writes over a lit pixel.
       This code tests `fb` directly. It does not test the output that it just
       wrote. Two adjacent lit source pixels must never make each other darker. */
    for (int row = 0; row < PSEMU_LCD_HEIGHT - 1; row++) {
        for (int col = 0; col < PSEMU_LCD_WIDTH; col++) {
            if (lcd_bit_on(fb, row, col) && !lcd_bit_on(fb, row + 1, col)) {
                pixels[(row + 1) * PSEMU_LCD_WIDTH + col] = shadow_rgba;
            }
        }
    }
}

/* A quarter of a megabyte of card copies. This data stays outside the stack frame of main. There is
   one such structure for each process, and an IR link pair is two processes, each with its own
   card. */
static content_writeback_t content_writeback;

int main(int argc, char **argv) {
    char exe_dir[900];
    char settings_config_path[1024];
    /* These buffers are writable. They are not const char * pointers into argv.
       File > Load BIOS... and File > Open App can write the new path into the
       same buffer after a successful load.
       The "bios:" and "app:" lines of the diagnostic report use these
       buffers. */
    char bios_path[1024];
    char app_path[1024];
    const char *positional[2];
    int npositional = 0;
    int saw_console_flag = 0;
    int saw_no_console_flag = 0;
    int show_console;
    app_settings_t settings;
    int i;

    {
        /* Registers SysLink, the clickable repository link in Help > About.
           This call has no effect for each other dialog in this file.
           One call here is simpler than a separate call at each
           DialogBoxParamA call. */
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_LINK_CLASS;
        InitCommonControlsEx(&icc);
    }

    /* This code calculates this path for each form of the paths and flags below.
       The settings file is always next to the executable. It is not next to the
       content path that the user supplies on the command line. */
    get_exe_dir(argv[0], exe_dir, sizeof(exe_dir));
    join_path(settings_config_path, sizeof(settings_config_path), exe_dir, SETTINGS_CONFIG_NAME);
    if (!load_settings(&settings, settings_config_path)) {
        /* This is the first run: settings.cfg did not exist.
           Write the file immediately, with the default values that load_settings
           supplied. hardware_id is one example.
           Do not wait for a different change, for example a BIOS load or a
           hardware ID edit, to cause the first save.
           This code does not change an existing file, even if a field in that
           file is empty.
           This app writes a field again only at a true change, the same as each
           other value in settings.cfg. */
        save_settings(&settings, settings_config_path);
    }

    /* --console and --no-console are the only flags. Each other argument is a
       positional argument.
       This code ignores each positional argument after the first two, and gives
       no message.
       This behavior agrees with the established behavior of this parser, which
       reads only the first two positional arguments. */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--console") == 0) {
            saw_console_flag = 1;
        } else if (strcmp(argv[i], "--no-console") == 0) {
            saw_no_console_flag = 1;
        } else if (npositional < 2) {
            positional[npositional++] = argv[i];
        }
    }
    /* An explicit flag always has priority, but only for this run.
       Without either flag, this code uses the stored preference from
       settings.cfg. That value is 0 if no settings file exists.
       A --console or --no-console flag for one run must not write settings.cfg
       again without a message. That file holds only a choice that the user made
       to keep. It does not hold a flag from one command line.
       This app has no control for this value. To change it permanently, a user
       must edit the show_console line in settings.cfg directly. */
    show_console = saw_console_flag ? 1 : (saw_no_console_flag ? 0 : settings.show_console);

    if (show_console) {
        /* The build makes this app a GUI-subsystem executable (see
           add_executable(... WIN32 ...) in CMakeLists.txt).
           The app has no console by default. Thus each fprintf(stderr, ...) call
           below goes to no visible location.
           --console gives the app a console, for a person who must see this
           output, for example during a test from a terminal. */
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }

    if (npositional >= 2) {
        snprintf(bios_path, sizeof(bios_path), "%s", positional[0]);
        snprintf(app_path, sizeof(app_path), "%s", positional[1]);
    } else if (npositional == 0) {
        /* No positional arguments means that a user started the executable with a
           double click in the file manager. It does not mean a start from a
           terminal.
           There is no command line for a path. Thus this code first uses the BIOS
           path in settings.cfg, from an earlier File > Open or command-line
           argument. If that path is absent, it uses a BIOS dump next to the
           executable.
           settings.cfg does not hold the memory-card path or the app path. It
           holds only the BIOS path (see its comment above).
           For the app path, this code always uses the file next to the
           executable. */
        if (settings.bios_path[0] != '\0') {
            snprintf(bios_path, sizeof(bios_path), "%s", settings.bios_path);
        } else {
            join_path(bios_path, sizeof(bios_path), exe_dir, "bios.bin");
        }
        join_path(app_path, sizeof(app_path), exe_dir, "memcard.mcr");
    } else {
        fprintf(stderr, "usage: %s [--console|--no-console] <bios.bin> <app.pss | app.mcs | memory-card.mcr>\n",
            argv[0]);
        fprintf(
            stderr,
            "  a %d-byte file is loaded as a raw memory card image (with its own directory) -\n"
            "  navigate and launch apps from the real BIOS menu with the keyboard, same as real\n"
            "  hardware. Anything else is loaded as a single Title Sector app (MCX0/MCX1), tried\n"
            "  either bare (.pss) or wrapped in a single-save directory frame (.mcs).\n",
            PSEMU_FLASH_SIZE);
        return 1;
    }

    /* A BIOS file, app file, or card file that is absent or invalid does not stop
       this app.
       File > Load BIOS... and File > Open App/Card... in the menu bar let the
       user select a file after the window opens.
       This app starts in both conditions, and psemu then has no loaded file.
       psemu_run does nothing until a BIOS loads (see the !ps->has_bios test in
       psemu_run). */
    size_t bios_size = 0, app_size = 0;
    uint8_t *bios = read_file(bios_path, &bios_size);
    uint8_t *app = read_file(app_path, &app_size);
    if (!bios) {
        fprintf(stderr, "psemu: couldn't read a BIOS dump at %s - launching anyway; use File > Load BIOS...\n",
            bios_path);
    }
    if (!app) {
        fprintf(stderr,
            "psemu: couldn't read an app or memory-card image at %s - launching anyway; use File > Open "
            "App/Card...\n",
            app_path);
    }

    psemu_t *ps = psemu_create();
    if (settings.hardware_id[0] != '\0') {
        uint32_t id;
        if (psemu_parse_hardware_id(settings.hardware_id, &id)) {
            psemu_set_hardware_id(ps, id);
        } else {
            fprintf(stderr,
                "psemu: couldn't parse hardware ID from %s (expected exactly 8 hex digits, e.g. \"EEEEEEEE\") - "
                "ignoring, using the default\n",
                settings_config_path);
        }
    }

    /* The Light, Dark, Classic, and Advanced Colors... selections all use this
       code at the next start (see save_settings above).
       load_settings always writes a real value into both fields. For a new
       settings.cfg file, that value is the Classic scheme.
       This Light default is important only if this code cannot parse the content
       of settings.cfg. An incorrect manual edit is one cause. */
    uint32_t pixel_rgba = DISPLAY_PIXEL_LIGHT;
    uint32_t bg_rgba = DISPLAY_BG_LIGHT;
    {
        uint8_t r, g, b;
        if (settings.pixel_color[0] != '\0' && parse_hex_rgb(settings.pixel_color, &r, &g, &b)) {
            pixel_rgba = RGBA_PACK(r, g, b);
        }
        if (settings.bg_color[0] != '\0' && parse_hex_rgb(settings.bg_color, &r, &g, &b)) {
            bg_rgba = RGBA_PACK(r, g, b);
        }
    }
    int show_shadows = settings.show_shadows;
    uint32_t shadow_rgba = DISPLAY_SHADOW_COLOR;
    {
        uint8_t r, g, b;
        if (settings.shadow_color[0] != '\0' && parse_hex_rgb(settings.shadow_color, &r, &g, &b)) {
            shadow_rgba = RGBA_PACK(r, g, b);
        }
    }

    if (bios && psemu_load_bios(ps, bios, bios_size) != PSEMU_OK) {
        fprintf(stderr, "psemu: %s isn't a valid BIOS image (expected %d bytes) - ignoring; use File > Load BIOS...\n",
            bios_path, PSEMU_BIOS_SIZE);
        free(bios);
        bios = NULL;
        bios_size = 0;
    }
    if (app && psemu_load_content(ps, app, app_size) != PSEMU_OK) {
        fprintf(stderr,
            "psemu: %s isn't a valid app or memory-card image - ignoring; use File > Open App/Card...\n", app_path);
        free(app);
        app = NULL;
        app_size = 0;
    }
    psemu_reset(ps);
    content_writeback_arm(&content_writeback, ps, app_path, app, app ? app_size : 0);

    /* Write the path of a BIOS that loaded correctly immediately. Do this for a
       path from a command-line argument, for a path from settings.cfg, and for
       the default path next to the executable.
       Do not wait for the exit. See the comment on the settings file above for
       the reason.
       A failed load does not change settings.bios_path. */
    if (bios) {
        snprintf(settings.bios_path, sizeof(settings.bios_path), "%s", bios_path);
        save_settings(&settings, settings_config_path);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* A user can change the size of this window.
       The 32x32 framebuffer fills the full render target at each frame, for each
       target size. The NULL dstrect argument of render_copy below does this.
       Thus a change to any size operates with no extra code.
       View > Native Size and View > Double Size are only a method to return to a
       known size. They are not the only sizes that this app supports. */
    SDL_Window *window = SDL_CreateWindow("pokketstation", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        PSEMU_LCD_WIDTH * SCALE, PSEMU_LCD_HEIGHT * SCALE, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, PSEMU_LCD_WIDTH, PSEMU_LCD_HEIGHT);

    SDL_AudioSpec audio_spec;
    SDL_zero(audio_spec);
    audio_spec.freq = PSEMU_AUDIO_SAMPLE_RATE_HZ;
    audio_spec.format = AUDIO_S16SYS;
    audio_spec.channels = 1;
    audio_spec.samples = 512;
    SDL_AudioDeviceID audio_dev = SDL_OpenAudioDevice(NULL, 0, &audio_spec, NULL, 0);
    if (audio_dev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s (continuing without sound)\n", SDL_GetError());
    } else {
        SDL_PauseAudioDevice(audio_dev, 0);
    }

    int running = 1;
    int cpu_faulted_reported = 0;

    ir_link_t ir_link;
    ir_link_init(&ir_link);
    /* After ir_link_init, which zeroes the struct. Off unless settings.cfg asks for it. */
    ir_link.show_diagnostics = settings.ir_link_diagnostics;

    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (!SDL_GetWindowWMInfo(window, &wm_info)) {
        fprintf(stderr, "SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        return 1;
    }
    HWND hwnd = wm_info.info.win.window;

    /* SDL registers its own window class with no icon.
       The resource icon of the executable (see resource.rc) is already the icon
       in the taskbar and the file manager. But that icon does not appear on the
       title bar or in the application switcher.
       Thus this code loads the icon at both sizes that Windows requests, and sets
       the icon on the window directly. */
    HICON icon_big = (HICON)LoadImageA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDI_MAINICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON icon_small = (HICON)LoadImageA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDI_MAINICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon_big);
    SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon_small);

    HMENU menu = LoadMenuA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDR_MAINMENU));
    RECT client_before;
    GetClientRect(hwnd, &client_before);
    SetMenu(hwnd, menu);
    /* SetMenu makes the client area smaller, to give space to the menu bar.
       The window must become larger to compensate for that reduction.
       Thus this code increases the window size by the exact reduction of the
       client area. This keeps the emulator at its native SCALE size. The
       framebuffer is not cut, and it has no border. */
    RECT client_after;
    GetClientRect(hwnd, &client_after);
    int shrink =
        (client_before.bottom - client_before.top) - (client_after.bottom - client_after.top);
    if (shrink > 0) {
        RECT window_rect;
        GetWindowRect(hwnd, &window_rect);
        /* SWP_FRAMECHANGED makes Windows calculate the cached item rectangles of
           the menu bar again, for the new window size.
           Without this flag, the menu bar keeps the geometry from the SetMenu
           call, before this size change.
           The first selection on a menu then opens its list with the old
           coordinates. The list opens to the left, and not to the right.
           That incorrect direction continues until a later pointer movement or
           selection causes a new calculation. */
        SetWindowPos(hwnd, NULL, 0, 0, window_rect.right - window_rect.left,
            (window_rect.bottom - window_rect.top) + shrink, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    /* The live map from a key to a PocketStation button. The main loop reads this
       map at each frame.
       The order is fixed: Up, Down, Left, Right, Fire, Create Debug Log, Reset,
       Quick Save State, and Quick Load State.
       This order agrees with IDC_REMAP_LABEL_BASE, IDC_REMAP_CHANGE_BASE, and
       save_key_bindings.
       This array is not const and not static: Tools > Remap Controls... changes
       the entries through menu_ctx.button_scancodes, which points to this same
       array.
       The last 4 entries (Create Debug Log, Reset, Save State, and Load State)
       are not PocketStation buttons. Their bit value is 0, thus the loop below
       ORs them into the button mask with no effect.
       The SDL_KEYDOWN tests below use their scancodes. Each test causes its
       action at a real key-down edge, one time for each press. It does not cause
       the action at each frame while the user holds the key. */
    button_binding_t button_scancodes[REMAP_BINDING_COUNT] = {
        {resolve_key_binding(settings.key_up, SDL_SCANCODE_UP), PSEMU_BUTTON_UP, "Up"},
        {resolve_key_binding(settings.key_down, SDL_SCANCODE_DOWN), PSEMU_BUTTON_DOWN, "Down"},
        {resolve_key_binding(settings.key_left, SDL_SCANCODE_LEFT), PSEMU_BUTTON_LEFT, "Left"},
        {resolve_key_binding(settings.key_right, SDL_SCANCODE_RIGHT), PSEMU_BUTTON_RIGHT, "Right"},
        {resolve_key_binding(settings.key_fire, SDL_SCANCODE_Z), PSEMU_BUTTON_FIRE, "Fire/Action"},
        {resolve_key_binding(settings.key_debug_log, SDL_SCANCODE_F12), 0, "Create Debug Log"},
        {resolve_key_binding(settings.key_reset, SDL_SCANCODE_F8), 0, "Reset"},
        {resolve_key_binding(settings.key_quick_save, SDL_SCANCODE_F5), 0, "Save State"},
        {resolve_key_binding(settings.key_quick_load, SDL_SCANCODE_F9), 0, "Load State"},
    };

    fprintf(stderr, "psemu: press %s at any time to write a diagnostic report to a pokketstation_report_*.log file\n",
        SDL_GetScancodeName(button_scancodes[5].scancode));

    menu_context_t menu_ctx;
    menu_ctx.ps = ps;
    menu_ctx.bios = &bios;
    menu_ctx.bios_size = &bios_size;
    menu_ctx.bios_path = bios_path;
    menu_ctx.bios_path_cap = sizeof(bios_path);
    menu_ctx.app = &app;
    menu_ctx.app_size = &app_size;
    menu_ctx.app_path = app_path;
    menu_ctx.app_path_cap = sizeof(app_path);
    menu_ctx.hwnd = hwnd;
    menu_ctx.running = &running;
    menu_ctx.cpu_faulted_reported = &cpu_faulted_reported;
    menu_ctx.pixel_rgba = &pixel_rgba;
    menu_ctx.bg_rgba = &bg_rgba;
    menu_ctx.show_shadows = &show_shadows;
    menu_ctx.button_scancodes = button_scancodes;
    menu_ctx.shadow_rgba = &shadow_rgba;
    menu_ctx.settings = &settings;
    menu_ctx.settings_path = settings_config_path;
    menu_ctx.exe_dir = exe_dir;
    menu_ctx.ir_link = &ir_link;
    menu_ctx.content_writeback = &content_writeback;
    SDL_SetWindowsMessageHook(handle_windows_message, &menu_ctx);

    uint32_t pixels[PSEMU_LCD_WIDTH * PSEMU_LCD_HEIGHT];
    int16_t audio_buf[1024];
    /* Lives out here, not inside the loop: its history has to carry across
       frames. See speaker_filter_t. */
    speaker_filter_t speaker_filter;
    speaker_filter_init(&speaker_filter);
    unsigned long frame = 0;

    /* The minimum number of frames that a button reads as pressed. The count
       starts at the press.
       This minimum makes a short real press as long as the duration that a
       scripted headless test confirms. The real BIOS accepts that duration
       reliably.
       At 32Hz, a real press of approximately 40ms is only approximately 1.3
       frames.
       A press between two SDL_GetKeyboardState calls can be visible to this
       emulator for only a small part of one frame. That part is too short, and
       the input code of the BIOS does not count it as a complete press.

       This value is a *minimum*. It is not an extension. If a user holds a key
       for a longer time, the core sees the release in the same frame as the real
       key release, and this code adds no time at the end. This difference is
       important. This code loaded a 5-frame counter again at each frame while the
       key was down. That method gave a fixed 5-frame tail after the release, and
       not a minimum. Thus each press, at each length, continued 5 frames after
       the key.

       The usable window is narrow. tools/button_timing_probe.c measures both of
       its limits against the real BIOS and a real app, in emulated real time and
       not in frames:
       - The BIOS browse screen ignores an Action press of less than
         approximately 35ms. That limit is near to the real hardware press of
         approximately 40ms in docs/app-notes.md. Thus the real BIOS operates
         normally, and this emulator has no timing error.
       - An app with an exit screen returns control to that browse screen
         approximately 62ms to 94ms after the press. The exact time depends on the
         position of the press in the tick of the app. If Action is still asserted
         at that moment, the browse screen reads a new press on the app that it
         shows. It then starts that app again immediately.
       Thus a press must be longer than approximately 35ms, and it must end
       before approximately 62ms.

       This loop can use only full frames, and a frame is 31.25ms. Thus the
       available durations are 31ms, 62ms, 94ms, and longer. Exactly one duration
       is in the window, and the test sweep confirms this: at 2 frames the exit
       operates correctly at all 64 tested timing offsets. At 3 frames, only 27
       offsets operate correctly, and at 4 frames only 1 offset. Below 2 frames,
       the boot navigation of the BIOS (Down, Action, Right, Action) never
       registers, and it never gets to an app.

       Thus an increase to this value is not a free safety margin. An increase
       broke the exit function. See also tools/pk_exit_test.c, which covers the
       app-side half of the same hazard. */
#define BUTTON_MIN_PRESS_FRAMES 2
    int frames_pressed[sizeof(button_scancodes) / sizeof(button_scancodes[0])] = {0};

    /* Show the settings from settings.cfg before a user opens the menu. After
       this call, each command keeps the menu in agreement with the state. See
       sync_menu_state. */
    sync_menu_state(&menu_ctx);

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == button_scancodes[5].scancode) {
                /* A report that the user requests, for a manual test.
                   Press this key immediately when a condition looks incorrect. A frozen
                   screen, no sound, and incorrect graphics are examples. Press the key
                   for each such condition, and not only for a CPU fault.
                   Tools > Remap Controls... can change this key. The default is F12.
                   See button_scancodes above. */
                char reason[64];
                snprintf(reason, sizeof(reason), "manual (%s)", SDL_GetScancodeName(button_scancodes[5].scancode));
                write_diagnostic_report(ps, reason, frame, bios_path, app_path);
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == button_scancodes[6].scancode) {
                /* Remappable via Tools > Remap Controls..., default F8. See button_scancodes above. */
                reset_emulation(&menu_ctx);
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == button_scancodes[7].scancode) {
                /* Tools > Remap Controls... can change this key. The default is F5.
                   See button_scancodes above.
                   This key uses slot 0, the quick slot. Only that slot has a key
                   binding. Thus an incorrect key press cannot write over slot 1 or
                   slot 2. */
                save_state_to_slot(&menu_ctx, 0);
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == button_scancodes[8].scancode) {
                /* Remappable via Tools > Remap Controls..., default F9. See button_scancodes above. */
                load_state_from_slot(&menu_ctx, 0);
            }
        }

        const uint8_t *keys = SDL_GetKeyboardState(NULL);
        uint32_t buttons = 0;
        size_t bi;
        for (bi = 0; bi < sizeof(button_scancodes) / sizeof(button_scancodes[0]); bi++) {
            int held = keys[button_scancodes[bi].scancode] != 0;
            /* A press that is shorter than the minimum continues to read as
               pressed. frames_pressed[bi] stays at 0 until a real key-down
               event occurs. Thus this code never makes a press for a key that
               was never down. The counter stops at the minimum, thus a long
               hold adds no time after the release. */
            int pressed = held || (frames_pressed[bi] > 0 && frames_pressed[bi] < BUTTON_MIN_PRESS_FRAMES);
            if (pressed) {
                if (frames_pressed[bi] < BUTTON_MIN_PRESS_FRAMES) {
                    frames_pressed[bi]++;
                }
                buttons |= button_scancodes[bi].bit;
            } else {
                frames_pressed[bi] = 0;
            }
        }
        psemu_set_buttons(ps, buttons);

        /* This budget is 33000 cycles for each frame, at a 32Hz refresh rate. The
           effective rate is approximately 1.056MHz.
           This value reverses an earlier change that made the budget agree with
           RTC_TICK_CYCLES in rtc.h (approximately 4MHz). That earlier change was
           an assumption with no validation: it matched one uncalibrated constant
           to another.
           A test on real hardware showed that the earlier value made the
           on-screen blink rate too fast.
           Real hardware operates at a variable clock rate, to a maximum of
           approximately 7.995MHz (see the CPU_FREQ table in core/src/clk.c, and
           docs/hardware-notes.md, "CLK_MODE").
           psemu_run scales its total throughput by two values: the CLK_MODE value
           that the app programs, and the real cycle cost of each instruction (see
           "Memory access timing" in docs/hardware-notes.md).
           This fixed budget for each frame is a real-time reference rate. It is
           not an approximation of an instruction count.
           This code keeps 33000 cycles for each frame because a test on real
           hardware confirms that the result is correct. The value has no
           independent derivation.
           See PSEMU_ASSUMED_CPU_HZ in dac.h for the applicable audio-rate
           conversion (33000 * 32). If this value changes, change that value to
           agree with it. */
        /* If the CPU meets an opcode that this emulator does not recognize, the
           register state and the memory state have no more meaning.
           A real, confirmed fault that this method found (see
           docs/hardware-notes.md, "Known open questions") gets to this point after
           approximately 1.3 billion instructions of correct operation.
           Do not assume that this state is safe because the fault has not occurred
           yet.
           Stop the CPU when this flag becomes set, and hold the last correct
           frame.
           Do not continue to execute incorrect data, and do not do this without a
           message.
           Before this correction, such a continuation looked to a user like an
           unexplained stop or crash, with no diagnostic data. */
        /* Tools > Date/Time Override.
           This setting is in emulated RAM, and the system menus of the BIOS also
           write to that RAM. Thus, to hold a value, this app must write the value
           again. One write is not sufficient. A write at each frame here also
           covers a reset, the startup, and a state load, with no extra code.
           There is no event to miss, because the next frame corrects each change.

           The cost is a few byte stores against the approximately 26,500
           instructions that a frame already emulates.

           psemu_settings_offsets_known guards this code. Thus this app makes no
           change on a BIOS revision that nobody traced. This agrees with the
           disabled menu items.

           psemu_app_running also guards this code, because a write at each frame
           is correct only while the BIOS shell owns that RAM. When the BIOS
           dispatches an app from the card, those two bytes belong to the app. A
           write to them 32 times each second then corrupts the app. With one
           real trading-card app, this was sufficient to make the app reject the
           PS1 save on the card and show its "ODD DATA" screen. That result is how
           this project found the fault. The override starts again when the app
           returns control. */
        if (psemu_settings_offsets_known(ps) && settings.datetime_override == DATETIME_OVERRIDE_OS &&
            !psemu_app_running(ps)) {
            SYSTEMTIME now;
            GetLocalTime(&now);
            /* In SYSTEMTIME, wDayOfWeek gives 0 for Sunday. The RTC field
               gives 1 for Sunday. psemu_set_datetime refuses the call while
               the BIOS programs the clock, and the next frame tries again. */
            psemu_set_datetime(
                ps, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, (int)now.wDayOfWeek + 1);
        }

        if (!psemu_cpu_faulted(ps)) {
            psemu_run(ps, 33000);
        } else if (!cpu_faulted_reported) {
            cpu_faulted_reported = 1;
            fprintf(
                stderr, "psemu: CPU hit an unrecognized opcode and has stopped - this is a real emulator bug, "
                        "not something you did. The game is frozen on its last good frame; please report this "
                        "along with what you were doing right before it happened.\n");
            write_diagnostic_report(ps, "cpu fault (unrecognized opcode)", frame, bios_path, app_path);
        }

        /* Moves the new TX edges of this frame onto the pipe.
           It also sends each RX edge that arrived from the other instance after the last frame.
           Those edges arrive in time for the psemu_run call of the next frame, which processes
           them through ir_tick.
           This is the same one-frame resolution, approximately 31ms, that each other input path
           uses. The buttons are one example. See ir_link.h.
           The status of ir_link can change here with no menu action: a peer connects, or the link
           fails. Thus this code writes the title again at each change of the status text. */
        {
            const char *status_before = ir_link_status_text(&ir_link);
            char status_before_copy[sizeof(((ir_link_t *)0)->status)];
            snprintf(status_before_copy, sizeof(status_before_copy), "%s", status_before);
            ir_link_pump(&ir_link, ps);
            if (strcmp(status_before_copy, ir_link_status_text(&ir_link)) != 0) {
                ir_link_refresh_title(&menu_ctx);
            }
        }

        if (audio_dev != 0) {
            uint32_t n = psemu_get_audio_samples(ps, audio_buf, sizeof(audio_buf) / sizeof(audio_buf[0]));
            if (n > 0) {
                /* Tools > Sound > Volume. This code reads the value at each frame,
                   thus a change takes effect at the next frame. This code always
                   drains the core, and it queues silence at 0%. If it did not queue
                   the silence, the data in the buffer would continue to play, and the
                   audio clock of this instance would become different from its
                   frames.
                   The speaker filter operates first. Thus the percentage stays a
                   simple loudness control above the output of the speaker. It does not
                   change how much the gain of the filter drives into the limiter of
                   that filter. */
                apply_speaker_filter(&speaker_filter, audio_buf, n, settings.speaker_sim);
                apply_master_volume(audio_buf, n, clamp_master_volume(settings.master_volume));
                SDL_QueueAudio(audio_dev, audio_buf, n * sizeof(audio_buf[0]));
            }
        }

        render_framebuffer(ps, pixels, pixel_rgba, bg_rgba, show_shadows, shadow_rgba);
        SDL_UpdateTexture(texture, NULL, pixels, PSEMU_LCD_WIDTH * sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        content_writeback_poll(&content_writeback, ps, frame);
        SDL_Delay(31); /* ~32Hz, matching the real LCD refresh */
        frame++;
    }

    /* Whatever is still inside the settle window when the user quits. */
    content_writeback_commit(&content_writeback, ps);
    ir_link_disconnect(&ir_link);

    if (audio_dev != 0) {
        SDL_CloseAudioDevice(audio_dev);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    /* This code does not save the settings.
       This app already wrote bios_path and hardware_id immediately at their last
       change (see the comment on the settings file above).
       This app never writes show_console. See the same comment.
       Thus there is nothing to write at the exit. */
    psemu_destroy(ps);
    free(bios);
    free(app);
    return 0;
}
