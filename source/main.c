#include <switch.h>

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define AVATAR_DIR      "sdmc:/avatars"
#define LOG_PATH        "sdmc:/avatars/avatar-tool.log"
#define REQUIRED_SIZE   256
#define MAX_USERS       ACC_USER_LIST_SIZE
#define VISIBLE_ROWS    30

#define CMD_ACC_SU_GET_PROFILE_EDITOR  205
#define CMD_EDITOR_STORE_WITH_IMAGE    101

static PadState g_pad;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    printf("%s\n", line);
    FILE *log = fopen(LOG_PATH, "a");
    if (log) {
        fprintf(log, "%s\n", line);
        fclose(log);
    }
    consoleUpdate(NULL);
}

static int pick(const char *title, const char **items, int count) {
    int sel = 0;
    for (;;) {
        int start = sel - VISIBLE_ROWS / 2;
        if (start > count - VISIBLE_ROWS) start = count - VISIBLE_ROWS;
        if (start < 0) start = 0;
        int end = start + VISIBLE_ROWS;
        if (end > count) end = count;

        consoleClear();
        printf("%s (%d/%d)\n\n", title, sel + 1, count);
        if (start > 0) printf("  ...\n");
        for (int i = start; i < end; i++)
            printf("%s %s\n", i == sel ? ">" : " ", items[i]);
        if (end < count) printf("  ...\n");
        printf("\n[A] select   [B] quit\n");
        consoleUpdate(NULL);

        while (appletMainLoop()) {
            padUpdate(&g_pad);
            u64 down = padGetButtonsDown(&g_pad);
            if (down & HidNpadButton_Down) { sel = (sel + 1) % count; break; }
            if (down & HidNpadButton_Up)   { sel = (sel + count - 1) % count; break; }
            if (down & HidNpadButton_A)    return sel;
            if (down & (HidNpadButton_B | HidNpadButton_Plus)) return -1;
        }
        if (!appletMainLoop()) return -1;
    }
}

static u8 *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }

    u8 *buf = malloc((size_t)len);
    if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *out_size = (size_t)len;
    return buf;
}

static int jpeg_info(const u8 *d, size_t n, int *w, int *h, int *progressive) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return -1;
    size_t i = 2;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) { i++; continue; }
        u8 marker = d[i + 1];
        if (marker == 0xFF) { i++; continue; }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) { i += 2; continue; }
        size_t seglen = ((size_t)d[i + 2] << 8) | d[i + 3];
        if (seglen < 2) return -1;
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (i + 9 > n) return -1;
            *h = (d[i + 5] << 8) | d[i + 6];
            *w = (d[i + 7] << 8) | d[i + 8];
            *progressive = (marker == 0xC2);
            return 0;
        }
        i += 2 + seglen;
    }
    return -1;
}

static Result getProfileEditor(AccountUid uid, Service *out) {
    return serviceDispatchIn(accountGetServiceSession(), CMD_ACC_SU_GET_PROFILE_EDITOR, uid,
        .out_num_objects = 1,
        .out_objects = out,
    );
}

static Result editorStoreWithImage(Service *editor, const AccountProfileBase *base,
                                   const AccountUserData *userdata, const void *img, size_t len) {
    return serviceDispatchIn(editor, CMD_EDITOR_STORE_WITH_IMAGE, *base,
        .buffer_attrs = {
            SfBufferAttr_In | SfBufferAttr_HipcPointer | SfBufferAttr_FixedSize,
            SfBufferAttr_In | SfBufferAttr_HipcMapAlias,
        },
        .buffers = {
            { userdata, sizeof(*userdata) },
            { img, len },
        },
    );
}

static Result apply_avatar(AccountUid uid, const u8 *jpg, size_t jpg_size) {
    AccountProfile profile;
    AccountProfileBase base;
    AccountUserData userdata;
    Service editor;
    Result rc;

    rc = accountGetProfile(&profile, uid);
    if (R_FAILED(rc)) { say("accountGetProfile failed: 0x%x", rc); return rc; }

    rc = accountProfileGet(&profile, &userdata, &base);
    accountProfileClose(&profile);
    if (R_FAILED(rc)) { say("accountProfileGet failed: 0x%x", rc); return rc; }

    rc = getProfileEditor(uid, &editor);
    if (R_FAILED(rc)) { say("GetProfileEditor failed: 0x%x", rc); return rc; }

    rc = editorStoreWithImage(&editor, &base, &userdata, jpg, jpg_size);
    serviceClose(&editor);
    if (R_FAILED(rc)) say("StoreWithImage failed: 0x%x", rc);
    return rc;
}

static void nickname_of(AccountUid uid, char *out, size_t out_size) {
    AccountProfile p;
    AccountProfileBase b;
    AccountUserData ud;
    if (R_FAILED(accountGetProfile(&p, uid))) return;
    if (R_SUCCEEDED(accountProfileGet(&p, &ud, &b)))
        snprintf(out, out_size, "%s", b.nickname);
    accountProfileClose(&p);
}

static int has_jpeg_extension(const char *name) {
    size_t l = strlen(name);
    return (l > 4 && !strcasecmp(name + l - 4, ".jpg")) ||
           (l > 5 && !strcasecmp(name + l - 5, ".jpeg"));
}

int main(void) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    char **files = NULL;
    int nfiles = 0;
    int cap = 0;

    say("avatar-tool starting");

    Result rc = accountInitialize(AccountServiceType_Administrator);
    if (R_FAILED(rc)) {
        say("accountInitialize failed: 0x%x", rc);
        goto done;
    }

    DIR *dir = opendir(AVATAR_DIR);
    if (!dir) { say("Cannot open " AVATAR_DIR " (create it and add a 256x256 .jpg)"); goto cleanup; }
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (!has_jpeg_extension(ent->d_name)) continue;
        if (nfiles == cap) {
            int ncap = cap ? cap * 2 : 32;
            char **grown = realloc(files, (size_t)ncap * sizeof(*files));
            if (!grown) break;
            files = grown;
            cap = ncap;
        }
        char *copy = strdup(ent->d_name);
        if (!copy) break;
        files[nfiles++] = copy;
    }
    closedir(dir);
    if (nfiles == 0) { say("No .jpg files found in " AVATAR_DIR); goto cleanup; }

    int fi = pick("Choose a 256x256 JPEG", (const char **)files, nfiles);
    if (fi < 0) goto cleanup;

    char path[256];
    snprintf(path, sizeof(path), AVATAR_DIR "/%s", files[fi]);

    size_t jpg_size = 0;
    u8 *jpg = read_file(path, &jpg_size);
    if (!jpg) { say("Could not read %s", path); goto cleanup; }

    int w = 0, h = 0, prog = 0;
    if (jpeg_info(jpg, jpg_size, &w, &h, &prog) != 0) { say("%s is not a valid JPEG", files[fi]); free(jpg); goto cleanup; }
    if (w != REQUIRED_SIZE || h != REQUIRED_SIZE)     { say("Image is %dx%d, must be %dx%d", w, h, REQUIRED_SIZE, REQUIRED_SIZE); free(jpg); goto cleanup; }
    if (prog)                                         { say("Progressive JPEG not supported; re-save as baseline"); free(jpg); goto cleanup; }

    AccountUid uid;
    memset(&uid, 0, sizeof(uid));
    char target[64] = "selected user";
    bool have_user = false;

    PselUserSelectionSettings psel;
    memset(&psel, 0, sizeof(psel));
    rc = pselShowUserSelector(&uid, &psel);
    if (R_SUCCEEDED(rc) && accountUidIsValid(&uid)) {
        have_user = true;
        nickname_of(uid, target, sizeof(target));
    } else {
        say("System user selector unavailable or cancelled (0x%x); using in-app list", rc);
    }

    if (!have_user) {
        AccountUid uids[MAX_USERS];
        s32 nusers = 0;
        rc = accountListAllUsers(uids, MAX_USERS, &nusers);
        if (R_FAILED(rc) || nusers == 0) { say("accountListAllUsers failed: 0x%x", rc); free(jpg); goto cleanup; }

        static char names[MAX_USERS][64];
        const char *name_ptrs[MAX_USERS];
        for (s32 i = 0; i < nusers; i++) {
            snprintf(names[i], sizeof(names[i]), "User %d", i + 1);
            nickname_of(uids[i], names[i], sizeof(names[i]));
            name_ptrs[i] = names[i];
        }

        int ui = pick("Apply to which user?", name_ptrs, nusers);
        if (ui < 0) { free(jpg); goto cleanup; }
        uid = uids[ui];
        snprintf(target, sizeof(target), "%s", names[ui]);
    }

    say("Applying %s to %s ...", files[fi], target);
    rc = apply_avatar(uid, jpg, jpg_size);
    free(jpg);
    if (R_SUCCEEDED(rc)) say("Avatar changed, please reboot for it to fully take effect.");

cleanup:
    accountExit();
done:
    for (int i = 0; i < nfiles; i++) free(files[i]);
    free(files);
    say("Press + to exit.");
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
