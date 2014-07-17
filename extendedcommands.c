#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include <signal.h>
#include <sys/wait.h>
#include <libgen.h>

#include "cutils/android_reboot.h"
#include "cutils/properties.h"
#include "make_ext4fs.h"

#include "voldclient/voldclient.h"
#include "bootloader.h"
#include "common.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "extendedcommands.h"
#include "advanced_functions.h"
#include "recovery_settings.h"
#include "nandroid.h"
#include "mtdutils/mounts.h"
#include "edify/expr.h"


#include "adb_install.h"

#ifdef PHILZ_TOUCH_RECOVERY
#include "libtouch_gui/gui_settings.h"
#endif

extern struct selabel_handle *sehandle;

int get_filtered_menu_selection(const char** headers, char** items, int menu_only, int initial_selection, int items_count) {
    int index;
    int offset = 0;
    int* translate_table = (int*)malloc(sizeof(int) * items_count);
    char* items_new[items_count];

    for (index = 0; index < items_count; index++) {
        items_new[index] = items[index];
    }

    for (index = 0; index < items_count; index++) {
        if (items_new[index] == NULL)
            continue;
        char *item = items_new[index];
        items_new[index] = NULL;
        items_new[offset] = item;
        translate_table[offset] = index;
        offset++;
    }
    items_new[offset] = NULL;

    initial_selection = translate_table[initial_selection];
    int ret = get_menu_selection(headers, items_new, menu_only, initial_selection);
    if (ret < 0 || ret >= offset) {
        free(translate_table);
        return ret;
    }

    ret = translate_table[ret];
    free(translate_table);
    return ret;
}

// returns negative value on failure and total bytes written on success
int write_string_to_file(const char* filename, const char* string) {
    char tmp[PATH_MAX];
    int ret = -1;

    ensure_path_mounted(filename);
    sprintf(tmp, "mkdir -p $(dirname %s)", filename);
    __system(tmp);
    FILE *file = fopen(filename, "w");
    if (file != NULL) {
        ret = fprintf(file, "%s", string);
        fclose(file);
    } else
        LOGE("Cannot write to %s\n", filename);

    return ret;
}

// called on recovery exit
// data will be mounted by call to write_string_to_file() on /data/media devices
// we need to ensure a proper unmount
void write_recovery_version() {
    char path[PATH_MAX];
    sprintf(path, "%s/%s", get_primary_storage_path(), RECOVERY_VERSION_FILE);
    write_string_to_file(path, EXPAND(RECOVERY_VERSION) "\n" EXPAND(TARGET_DEVICE));
    // force unmount /data for /data/media devices as we call this on recovery exit
    preserve_data_media(0);
    ensure_path_unmounted(path);
    preserve_data_media(1);
}

static void write_last_install_path(const char* install_path) {
    char path[PATH_MAX];
    sprintf(path, "%s/%s", get_primary_storage_path(), RECOVERY_LAST_INSTALL_FILE);
    write_string_to_file(path, install_path);
}

const char* read_last_install_path() {
    static char path[PATH_MAX];
    sprintf(path, "%s/%s", get_primary_storage_path(), RECOVERY_LAST_INSTALL_FILE);

    ensure_path_mounted(path);
    FILE *f = fopen(path, "r");
    if (f != NULL) {
        fgets(path, PATH_MAX, f);
        fclose(f);

        return path;
    }
    return NULL;
}

void toggle_signature_check() {
    char value[3];
    signature_check_enabled.value = !signature_check_enabled.value;
    sprintf(value, "%d", signature_check_enabled.value);
    write_config_file(PHILZ_SETTINGS_FILE, signature_check_enabled.key, value);
    // ui_print("Signature Check: %s\n", signature_check_enabled.value ? "Enabled" : "Disabled");
}

static void toggle_install_zip_verify_md5() {
    char value[3];
    install_zip_verify_md5.value ^= 1;
    sprintf(value, "%d", install_zip_verify_md5.value);
    write_config_file(PHILZ_SETTINGS_FILE, install_zip_verify_md5.key, value);
}

#ifdef ENABLE_LOKI
static void toggle_loki_support() {
    char value[3];
    apply_loki_patch.value ^= 1;
    sprintf(value, "%d", apply_loki_patch.value);
    write_config_file(PHILZ_SETTINGS_FILE, apply_loki_patch.key, value);
    // ui_print("Loki Support: %s\n", apply_loki_patch.value ? "Enabled" : "Disabled");
}

// this is called when we load recovery settings and when we istall_package()
// it is needed when after recovery is booted, user wipes /data, then he installs a ROM: we can still return the user setting 
int loki_support_enabled() {
    char no_loki_variant[PROPERTY_VALUE_MAX];
    int ret = -1;

    property_get("ro.loki_disabled", no_loki_variant, "0");
    if (strcmp(no_loki_variant, "0") == 0) {
        // device variant supports loki: check if user enabled it
        // if there is no settings file (read_config_file() < 0), it could be we have wiped /data before installing zip
        // in that case, return current value (we last loaded on start or when user last set it) and not default
        if (read_config_file(PHILZ_SETTINGS_FILE, apply_loki_patch.key, no_loki_variant, "0") >= 0) {
            if (strcmp(no_loki_variant, "true") == 0 || strcmp(no_loki_variant, "1") == 0)
                apply_loki_patch.value = 0;
            else
                apply_loki_patch.value = 1;
        }
        ret = apply_loki_patch.value;
    }
    return ret;
}
#endif

// top fixed menu items, those before extra storage volumes
#define FIXED_TOP_INSTALL_ZIP_MENUS 1
// bottom fixed menu items, those after extra storage volumes
#define FIXED_BOTTOM_INSTALL_ZIP_MENUS 7
#define FIXED_INSTALL_ZIP_MENUS (FIXED_TOP_INSTALL_ZIP_MENUS + FIXED_BOTTOM_INSTALL_ZIP_MENUS)

int show_install_update_menu() {
    char buf[100];
    int i = 0, chosen_item = 0;
    // + 1 for last NULL item
    char* install_menu_items[MAX_NUM_MANAGED_VOLUMES + FIXED_INSTALL_ZIP_MENUS + 1];

    char* primary_path = get_primary_storage_path();
    char** extra_paths = get_extra_storage_paths();
    int num_extra_volumes = get_num_extra_volumes();

    memset(install_menu_items, 0, sizeof(install_menu_items));

    const char* headers[] = { "Install update from zip file", "", NULL };

    // FIXED_TOP_INSTALL_ZIP_MENUS
    sprintf(buf, "Choose zip from %s", primary_path);
    install_menu_items[0] = strdup(buf);

    // extra storage volumes (vold managed)
    for (i = 0; i < num_extra_volumes; i++) {
        sprintf(buf, "Choose zip from %s", extra_paths[i]);
        install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + i] = strdup(buf);
    }

    // FIXED_BOTTOM_INSTALL_ZIP_MENUS
    char item_toggle_signature_check[MENU_MAX_COLS] = "";
    char item_install_zip_verify_md5[MENU_MAX_COLS] = "";
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes]     = "Choose zip Using Free Browse Mode";
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 1] = "Choose zip from Last Install Folder";
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 2] = "Install zip from sideload";
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 3] = "Install Multiple zip Files";
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 4] = item_toggle_signature_check;
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 5] = item_install_zip_verify_md5;
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 6] = "Setup Free Browse Mode";

    // extra NULL for GO_BACK
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + FIXED_BOTTOM_INSTALL_ZIP_MENUS] = NULL;

    for (;;) {
        if (signature_check_enabled.value)
            ui_format_gui_menu(item_toggle_signature_check, "Signature Verification", "(x)");
        else ui_format_gui_menu(item_toggle_signature_check, "Signature Verification", "( )");

        if (install_zip_verify_md5.value)
            ui_format_gui_menu(item_install_zip_verify_md5, "Verify zip md5sum", "(x)");
        else ui_format_gui_menu(item_install_zip_verify_md5, "Verify zip md5sum", "( )");

        chosen_item = get_menu_selection(headers, install_menu_items, 0, 0);
        if (chosen_item == 0) {
            show_choose_zip_menu(primary_path);
        } else if (chosen_item >= FIXED_TOP_INSTALL_ZIP_MENUS && chosen_item < FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes) {
            show_choose_zip_menu(extra_paths[chosen_item - FIXED_TOP_INSTALL_ZIP_MENUS]);
        } else if (chosen_item == FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes) {
            // browse for zip files up/backward including root system and have a default user set start folder
            if (show_custom_zip_menu() != 0)
                set_custom_zip_path();
        } else if (chosen_item == FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 1) {
            const char *last_path_used = read_last_install_path();
            if (last_path_used == NULL)
                show_choose_zip_menu(primary_path);
            else
                show_choose_zip_menu(last_path_used);
        } else if (chosen_item == FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 2) {
            enter_sideload_mode(INSTALL_SUCCESS);
        } else if (chosen_item == FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 3) {
            show_multi_flash_menu();
        } else if (chosen_item == FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 4) {
            toggle_signature_check();
        } else if (chosen_item == FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 5) {
            toggle_install_zip_verify_md5();
        } else if (chosen_item == FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 6) {
            set_custom_zip_path();
        } else {
            // GO_BACK or REFRESH (chosen_item < 0)
            goto out;
        }
    }
out:
    // free all the dynamic items
    free(install_menu_items[0]);
    if (extra_paths != NULL) {
        for (i = 0; i < num_extra_volumes; i++)
            free(install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + i]);
    }
    return chosen_item;
}

void free_string_array(char** array) {
    if (array == NULL)
        return;
    char* cursor = array[0];
    int i = 0;
    while (cursor != NULL) {
        free(cursor);
        cursor = array[++i];
    }
    free(array);
}

// to gather directories you need to pass NULL for fileExtensionOrDirectory
// else, only files are gathered. Pass "" to gather all files
// NO  MORE NEEDED: if it is not called by choose_file_menu(), passed directory MUST end with a trailing /
static int gather_hidden_files = 0;
void set_gather_hidden_files(int enable) {
    gather_hidden_files = enable;
}

char** gather_files(const char* basedir, const char* fileExtensionOrDirectory, int* numFiles) {
    DIR *dir;
    struct dirent *de;
    int total = 0;
    int i;
    char** files = NULL;
    int pass;
    *numFiles = 0;
    int dirLen = strlen(basedir);
    char directory[PATH_MAX];

    // Append a trailing slash if necessary
    strcpy(directory, basedir);
    if (directory[dirLen - 1] != '/') {
        strcat(directory, "/");
        ++dirLen;
    }

    dir = opendir(directory);
    if (dir == NULL) {
        ui_print("Couldn't open directory %s\n", directory);
        return NULL;
    }

    unsigned int extension_length = 0;
    if (fileExtensionOrDirectory != NULL)
        extension_length = strlen(fileExtensionOrDirectory);

    i = 0;
    // first pass (pass==0) only returns "total" valid file names to initialize files[total] size
    // second pass (pass == 1), rewinddir and initializes files[i] with directory contents
    for (pass = 0; pass < 2; pass++) {
        while ((de = readdir(dir)) != NULL) {
            // skip hidden files
            if (!gather_hidden_files && de->d_name[0] == '.')
                continue;

            // NULL means that we are gathering directories, so skip this
            if (fileExtensionOrDirectory != NULL) {
                if (strcmp("", fileExtensionOrDirectory) == 0) {
                    // we exclude directories since they are gathered on second call to gather_files() by choose_file_menu()
                    // and we keep stock behavior: folders are gathered only by passing NULL
                    // else, we break things at strcat(files[i], "/") in end of while loop
                    struct stat info;
                    char fullFileName[PATH_MAX];
                    strcpy(fullFileName, directory);
                    strcat(fullFileName, de->d_name);
                    lstat(fullFileName, &info);
                    // make sure it is not a directory
                    if (S_ISDIR(info.st_mode))
                        continue;
                } else {
                    // make sure that we can have the desired extension (prevent seg fault)
                    if (strlen(de->d_name) < extension_length)
                        continue;
                    // compare the extension
                    if (strcmp(de->d_name + strlen(de->d_name) - extension_length, fileExtensionOrDirectory) != 0)
                        continue;
                }
            } else {
                struct stat info;
                char fullFileName[PATH_MAX];
                strcpy(fullFileName, directory);
                strcat(fullFileName, de->d_name);
                lstat(fullFileName, &info);
                // make sure it is a directory
                if (!(S_ISDIR(info.st_mode)))
                    continue;
            }

            if (pass == 0) {
                total++;
                continue;
            }

            // only second pass (pass==1) reaches here: initializes files[i] with directory contents
            files[i] = (char*)malloc(dirLen + strlen(de->d_name) + 2);
            strcpy(files[i], directory);
            strcat(files[i], de->d_name);
            if (fileExtensionOrDirectory == NULL)
                strcat(files[i], "/");
            i++;
        }
        if (pass == 1)
            break;
        if (total == 0)
            break;
        // only first pass (pass == 0) reaches here. We rewinddir for second pass
        // initialize "total" with number of valid files to show and initialize files[total]
        rewinddir(dir);
        *numFiles = total;
        files = (char**)malloc((total + 1) * sizeof(char*));
        files[total] = NULL;
    }

    if (closedir(dir) < 0) {
        LOGE("Failed to close directory.\n");
    }

    if (total == 0) {
        return NULL;
    }

    // sort the result
    if (files != NULL) {
        for (i = 0; i < total; i++) {
            int curMax = -1;
            int j;
            for (j = 0; j < total - i; j++) {
                if (curMax == -1 || strcmpi(files[curMax], files[j]) < 0)
                    curMax = j;
            }
            char* temp = files[curMax];
            files[curMax] = files[total - i - 1];
            files[total - i - 1] = temp;
        }
    }

    return files;
}

// pass in NULL for fileExtensionOrDirectory and you will get a directory chooser
// pass in "" to gather all files without filtering extension or filename
// returned directory (when NULL is passed as file extension) has a trailing / causing a wired // return path
// choose_file_menu returns NULL when no file is found or if we choose no file in selection
// no_files_found = 1 when no valid file was found, no_files_found = 0 when we found a valid file
// WARNING : CALLER MUST ALWAYS FREE THE RETURNED POINTER
int no_files_found = 0;
char* choose_file_menu(const char* basedir, const char* fileExtensionOrDirectory, const char* headers[]) {
    const char* fixed_headers[20];
    int numFiles = 0;
    int numDirs = 0;
    int i;
    char* return_value = NULL;
    char directory[PATH_MAX];
    int dir_len = strlen(basedir);

    strcpy(directory, basedir);

    // Append a trailing slash if necessary
    if (directory[dir_len - 1] != '/') {
        strcat(directory, "/");
        dir_len++;
    }

    i = 0;
    while (headers[i]) {
        fixed_headers[i] = headers[i];
        i++;
    }
    fixed_headers[i] = directory;
    // let's spare some header space
    // fixed_headers[i + 1] = "";
    // fixed_headers[i + 2] = NULL;
    fixed_headers[i + 1] = NULL;

    char** files = gather_files(directory, fileExtensionOrDirectory, &numFiles);
    char** dirs = NULL;
    if (fileExtensionOrDirectory != NULL)
        dirs = gather_files(directory, NULL, &numDirs);
    int total = numDirs + numFiles;
    if (total == 0) {
        // we found no valid file to select
        no_files_found = 1;
        ui_print("No files found.\n");
    } else {
        // we found a valid file to select
        no_files_found = 0;
        char** list = (char**)malloc((total + 1) * sizeof(char*));
        list[total] = NULL;


        for (i = 0; i < numDirs; i++) {
            list[i] = strdup(dirs[i] + dir_len);
        }

        for (i = 0; i < numFiles; i++) {
            list[numDirs + i] = strdup(files[i] + dir_len);
        }

        for (;;) {
            int chosen_item = get_menu_selection(fixed_headers, list, 0, 0);
            if (chosen_item == GO_BACK || chosen_item == REFRESH)
                break;
            if (chosen_item < numDirs) {
                char* subret = choose_file_menu(dirs[chosen_item], fileExtensionOrDirectory, headers);
                if (subret != NULL) {
                    // we selected either a folder (or a file from a re-entrant call)
                    return_value = strdup(subret);
                    free(subret);
                    break;
                }
                // the previous re-entrant call did a GO_BACK, REFRESH or no file found in a directory: subret == NULL
                // we drop to up folder
                continue;
            }
            // we selected a file
            return_value = strdup(files[chosen_item - numDirs]);
            break;
        }
        free_string_array(list);
    }

    free_string_array(files);
    free_string_array(dirs);
    return return_value;
}

void show_choose_zip_menu(const char *mount_point) {
    if (ensure_path_mounted(mount_point) != 0) {
        LOGE("Can't mount %s\n", mount_point);
        return;
    }

    const char* headers[] = { "Choose a zip to apply", NULL };

    char* file = choose_file_menu(mount_point, ".zip", headers);
    if (file == NULL)
        return;

    char tmp[PATH_MAX];
    int yes_confirm;

    sprintf(tmp, "Yes - Install %s", BaseName(file));
    if (install_zip_verify_md5.value) start_md5_verify_thread(file);
    else start_md5_display_thread(file);

    yes_confirm = confirm_selection("Confirm install?", tmp);

    if (install_zip_verify_md5.value) stop_md5_verify_thread();
    else stop_md5_display_thread();

    if (yes_confirm) {
        install_zip(file);
        sprintf(tmp, "%s", DirName(file));
        write_last_install_path(tmp);
    }

    free(file);
}

void show_nandroid_restore_menu(const char* path) {
    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return;
    }

    const char* headers[] = { "Choose an image to restore", NULL };

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/backup/", path);
    char* file = choose_file_menu(tmp, NULL, headers);
    if (file == NULL)
        return;

    if (confirm_selection("Confirm restore?", "Yes - Restore"))
        nandroid_restore(file, 1, 1, 1, 1, 1, 0);

    free(file);
}

void show_nandroid_delete_menu(const char* volume_path) {
    if (ensure_path_mounted(volume_path) != 0) {
        LOGE("Can't mount %s\n", volume_path);
        return;
    }

    const char* headers[] = { "Choose a backup to delete", NULL };
    char path[PATH_MAX];
    char tmp[PATH_MAX];
    char* file;

    if (twrp_backup_mode.value) {
        char device_id[PROPERTY_VALUE_MAX];
        get_device_id(device_id);
        sprintf(path, "%s/%s/%s", volume_path, TWRP_BACKUP_PATH, device_id);
    } else {
        sprintf(path, "%s/%s", volume_path, CWM_BACKUP_PATH);    
    }

    for(;;) {
        file = choose_file_menu(path, NULL, headers);
        if (file == NULL)
            return;

        sprintf(tmp, "Yes - Delete %s", BaseName(file));
        if (confirm_selection("Confirm delete?", tmp)) {
            sprintf(tmp, "rm -rf '%s'", file);
            __system(tmp);
        }

        free(file);
    }
}

int confirm_selection(const char* title, const char* confirm) {
    // check if recovery needs no confirm, many confirm or a few confirm menus
    char path[PATH_MAX];
    int many_confirm;
    sprintf(path, "%s/%s", get_primary_storage_path(), RECOVERY_NO_CONFIRM_FILE);
    if (file_found(path))
        return 1;

    sprintf(path, "%s/%s", get_primary_storage_path(), RECOVERY_MANY_CONFIRM_FILE);
    many_confirm = file_found(path);

    char* confirm_str = strdup(confirm);
    const char* confirm_headers[] = { title, "  THIS CAN NOT BE UNDONE.", "", NULL };
    int ret = 0;

    int old_val = ui_is_showing_back_button();
    ui_set_showing_back_button(0);

    if (many_confirm) {
        char* items[] = {
            "No",
            "No",
            "No",
            "No",
            "No",
            "No",
            "No",
            confirm_str, //" Yes -- wipe partition",   // [7]
            "No",
            "No",
            "No",
            NULL
        };
        int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
        ret = (chosen_item == 7);
    } else {
        char* items[] = {
            "No",
            confirm_str, //" Yes -- wipe partition",   // [1]
            "No",
            NULL
        };
        int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
        ret = (chosen_item == 1);
    }

    free(confirm_str);
    ui_set_showing_back_button(old_val);
    return ret;
}

int confirm_with_headers(const char** confirm_headers, const char* confirm) {
    // check if recovery needs no confirm, many confirm or a few confirm menus
    char path[PATH_MAX];
    int many_confirm;
    sprintf(path, "%s/%s", get_primary_storage_path(), RECOVERY_NO_CONFIRM_FILE);
    if (file_found(path))
        return 1;

    sprintf(path, "%s/%s", get_primary_storage_path(), RECOVERY_MANY_CONFIRM_FILE);
    many_confirm = file_found(path);

    char* confirm_str = strdup(confirm);
    int ret = 0;

    int old_val = ui_is_showing_back_button();
    ui_set_showing_back_button(0);

    if (many_confirm) {
        char* items[] = {
            "No",
            "No",
            "No",
            "No",
            "No",
            "No",
            "No",
            confirm_str, //" Yes -- wipe partition",   // [7]
            "No",
            "No",
            "No",
            NULL
        };
        int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
        ret = (chosen_item == 7);
    } else {
        char* items[] = {
            "No",
            confirm_str, //" Yes -- wipe partition",   // [1]
            "No",
            NULL
        };
        int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
        ret = (chosen_item == 1);
    }

    free(confirm_str);
    ui_set_showing_back_button(old_val);
    return ret;
}

/****************************/
/* Format and mount options */
/****************************/
// mount usb storage
static int control_usb_storage(bool on) {
    int i = 0;
    int num = 0;

    for (i = 0; i < get_num_volumes(); i++) {
        Volume *v = get_device_volumes() + i;
        if (fs_mgr_is_voldmanaged(v) && vold_is_volume_available(v->mount_point)) {
            if (on) {
                vold_share_volume(v->mount_point);
            } else {
                vold_unshare_volume(v->mount_point, 1);
            }
            property_set("sys.storage.ums_enabled", on ? "1" : "0");
            num++;
        }
    }
    return num;
}

static void show_mount_usb_storage_menu() {
    // Enable USB storage using vold
    if (!control_usb_storage(true))
        return;

    const char* headers[] = {
        "USB Mass Storage device",
        "Leaving this menu unmounts",
        "your SD card from your PC.",
        "",
        NULL
    };

    char* list[] = { "Unmount", NULL };

    for (;;) {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK || chosen_item == 0)
            break;
    }

    // Disable USB storage
    control_usb_storage(false);
}

// ext4 <-> f2fs conversion
#ifdef USE_F2FS
static void format_ext4_or_f2fs(const char* volume) {
    if (is_data_media_volume_path(volume))
        return;

    Volume* v = volume_for_path(volume);
    if (v == NULL)
        return;

    const char* headers[] = { "Format device:", v->mount_point, "", NULL };

    char* list[] = {
        "default",
        "ext4",
        "f2fs",
        NULL
    };

    char cmd[PATH_MAX];
    char confirm[128];
    int ret = -1;
    int chosen_item = get_menu_selection(headers, list, 0, 0);

    if (chosen_item < 0) // REFRESH or GO_BACK
        return;

    sprintf(confirm, "Format %s (%s) ?", v->mount_point, list[chosen_item]);
    if (!confirm_selection(confirm, "Yes - Format device"))
        return;

    if (ensure_path_unmounted(v->mount_point) != 0)
        return;

    switch (chosen_item) {
        case 0:
            ret = format_volume(v->mount_point);
            break;
        case 1:
        case 2:
            ret = format_device(v->blk_device, v->mount_point, list[chosen_item]);
            break;
    }

    // refresh volume table (Volume*) and recreate the /etc/fstab file for proper system mount command function
    load_volume_table();
    setup_data_media(1);

    if (ret)
        LOGE("Could not format %s (%s)\n", volume, list[chosen_item]);
    else
        ui_print("Done formatting %s (%s)\n", volume, list[chosen_item]);
}
#endif

typedef struct {
    char mount[255];
    char unmount[255];
    char path[PATH_MAX];
} MountMenuEntry;

typedef struct {
    char txt[255];
    char path[PATH_MAX];
    char type[255];
} FormatMenuEntry;

typedef struct {
    char *name;
    int can_mount;
    int can_format;
} MFMatrix;

MFMatrix get_mnt_fmt_capabilities(char *fs_type, char *mount_point) {
    MFMatrix mfm = { mount_point, 1, 1 };

    const int NUM_FS_TYPES = 6;
    MFMatrix *fs_matrix = malloc(NUM_FS_TYPES * sizeof(MFMatrix));
    // Defined capabilities:   fs_type     mnt fmt
    fs_matrix[0] = (MFMatrix){ "bml",       0,  1 };
    fs_matrix[1] = (MFMatrix){ "datamedia", 0,  1 };
    fs_matrix[2] = (MFMatrix){ "emmc",      0,  1 };
    fs_matrix[3] = (MFMatrix){ "mtd",       0,  0 };
    fs_matrix[4] = (MFMatrix){ "ramdisk",   0,  0 };
    fs_matrix[5] = (MFMatrix){ "swap",      0,  0 };

    const int NUM_MNT_PNTS = 6;
    MFMatrix *mp_matrix = malloc(NUM_MNT_PNTS * sizeof(MFMatrix));
    // Defined capabilities:   mount_point   mnt fmt
    mp_matrix[0] = (MFMatrix){ "/misc",       0,  0 };
    mp_matrix[1] = (MFMatrix){ "/radio",      0,  0 };
    mp_matrix[2] = (MFMatrix){ "/bootloader", 0,  0 };
    mp_matrix[3] = (MFMatrix){ "/recovery",   0,  0 };
    mp_matrix[4] = (MFMatrix){ "/efs",        0,  0 };
    mp_matrix[5] = (MFMatrix){ "/wimax",      0,  0 };

    int i;
    for (i = 0; i < NUM_FS_TYPES; i++) {
        if (strcmp(fs_type, fs_matrix[i].name) == 0) {
            mfm.can_mount = fs_matrix[i].can_mount;
            mfm.can_format = fs_matrix[i].can_format;
        }
    }
    for (i = 0; i < NUM_MNT_PNTS; i++) {
        if (strcmp(mount_point, mp_matrix[i].name) == 0) {
            mfm.can_mount = mp_matrix[i].can_mount;
            mfm.can_format = mp_matrix[i].can_format;
        }
    }

    free(fs_matrix);
    free(mp_matrix);

    // User-defined capabilities
    char *custom_mp;
    char custom_forbidden_mount[PROPERTY_VALUE_MAX];
    char custom_forbidden_format[PROPERTY_VALUE_MAX];
    property_get("ro.cwm.forbid_mount", custom_forbidden_mount, "");
    property_get("ro.cwm.forbid_format", custom_forbidden_format, "");

    custom_mp = strtok(custom_forbidden_mount, ",");
    while (custom_mp != NULL) {
        if (strcmp(mount_point, custom_mp) == 0) {
            mfm.can_mount = 0;
        }
        custom_mp = strtok(NULL, ",");
    }

    custom_mp = strtok(custom_forbidden_format, ",");
    while (custom_mp != NULL) {
        if (strcmp(mount_point, custom_mp) == 0) {
            mfm.can_format = 0;
        }
        custom_mp = strtok(NULL, ",");
    }

    return mfm;
}

void show_partition_format_menu() {
    const char* headers[] = { "Format partitions menu", NULL };

    char* confirm_format = "Confirm format?";
    char* confirm = "Yes - Format";
    char confirm_string[255];

    FormatMenuEntry* format_menu = NULL;
    char* list[256];

    int i = 0;
    int formatable_volumes = 0;
    int num_volumes;
    int chosen_item = 0;

    num_volumes = get_num_volumes();

    if (!num_volumes) {
        LOGE("empty fstab list!\n");
        return;
    }

    format_menu = malloc(num_volumes * sizeof(FormatMenuEntry));

    for (i = 0; i < num_volumes; i++) {
        Volume* v = get_device_volumes() + i;

        if (fs_mgr_is_voldmanaged(v) && !vold_is_volume_available(v->mount_point)) {
            continue;
        }

        MFMatrix mfm = get_mnt_fmt_capabilities(v->fs_type, v->mount_point);

        if (mfm.can_format) {
            sprintf(format_menu[formatable_volumes].txt, "format %s", v->mount_point);
            sprintf(format_menu[formatable_volumes].path, "%s", v->mount_point);
            sprintf(format_menu[formatable_volumes].type, "%s", v->fs_type);
            ++formatable_volumes;
        }
    }

#ifdef USE_F2FS
    int enable_f2fs_ext4_conversion = 0;
#endif
    for (;;) {
        for (i = 0; i < formatable_volumes; i++) {
            FormatMenuEntry* e = &format_menu[i];
            list[i] = e->txt;
        }

        if (!is_data_media()) {
            list[formatable_volumes] = NULL;
#ifdef USE_F2FS
            list[formatable_volumes] = "toggle f2fs <-> ext4 migration";
            list[formatable_volumes + 1] = NULL;
#endif
        } else {
            list[formatable_volumes] = "format /data and /data/media (/sdcard)";
            list[formatable_volumes + 1] = NULL;
#ifdef USE_F2FS
            list[formatable_volumes + 1] = "toggle f2fs <-> ext4 migration";
            list[formatable_volumes + 2] = NULL;
#endif
        }

        chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item < 0)    // GO_BACK / REFRESH
            break;

        if (is_data_media() && chosen_item == formatable_volumes) {
            if (!confirm_selection("format /data and /data/media (/sdcard)", confirm))
                continue;
            preserve_data_media(0);
#ifdef USE_F2FS
            if (enable_f2fs_ext4_conversion) {
                format_ext4_or_f2fs("/data");
            } else
#endif
            {
                ui_print("Formatting /data...\n");
                if (0 != format_volume("/data"))
                    LOGE("Error formatting /data!\n");
                else
                    ui_print("Done.\n");
            }
            preserve_data_media(1);
            setup_data_media(1); // recreate /data/media with proper permissions, mount /data and unmount when done
        } else if (chosen_item < formatable_volumes) {
            FormatMenuEntry* e = &format_menu[chosen_item];
            sprintf(confirm_string, "%s - %s", e->path, confirm_format);

            // support user choice fstype when formatting external storage
            // ensure fstype==auto because most devices with internal vfat storage cannot be formatted to other types
            // if e->type == auto and it is not an extra storage, it will be wiped using format_volume() below (rm -rf like)
            if (strcmp(e->type, "auto") == 0) {
                Volume* v = volume_for_path(e->path);
                if (fs_mgr_is_voldmanaged(v) || can_partition(e->path)) {
                    show_format_sdcard_menu(e->path);
                    continue;
                }
            }

#ifdef USE_F2FS
            if (enable_f2fs_ext4_conversion && !(is_data_media() && strcmp(e->path, "/data") == 0)) {
                if (strcmp(e->type, "ext4") == 0 || strcmp(e->type, "f2fs") == 0) {
                    format_ext4_or_f2fs(e->path);
                    continue;
                } else {
                    ui_print("unsupported file system (%s)\n", e->type);
                }
            } else
#endif
            {
                if (!confirm_selection(confirm_string, confirm))
                    continue;
                ui_print("Formatting %s...\n", e->path);
                if (0 != format_volume(e->path))
                    LOGE("Error formatting %s!\n", e->path);
                else
                    ui_print("Done.\n");
            }
        }
#ifdef USE_F2FS
        else if ((is_data_media() && chosen_item == (formatable_volumes + 1)) ||
                    (!is_data_media() && chosen_item == (formatable_volumes))) {
            enable_f2fs_ext4_conversion ^= 1;
            ui_print("ext4 <-> f2fs conversion %s\n", enable_f2fs_ext4_conversion ? "enabled" : "disabled");
        }
#endif
    }

    free(format_menu);
}

int show_partition_mounts_menu() {
    const char* headers[] = { "Mounts and Storage Menu", NULL };

    MountMenuEntry* mount_menu = NULL;
    char* list[256];

    int i = 0;
    int mountable_volumes = 0;
    int num_volumes;
    int chosen_item = 0;

    num_volumes = get_num_volumes();

    if (!num_volumes) {
        LOGE("empty fstab list!\n");
        return GO_BACK;
    }

    mount_menu = malloc(num_volumes * sizeof(MountMenuEntry));

    for (i = 0; i < num_volumes; i++) {
        Volume* v = get_device_volumes() + i;

        if (fs_mgr_is_voldmanaged(v) && !vold_is_volume_available(v->mount_point)) {
            continue;
        }

        MFMatrix mfm = get_mnt_fmt_capabilities(v->fs_type, v->mount_point);

        if (mfm.can_mount) {
            sprintf(mount_menu[mountable_volumes].mount, "mount %s", v->mount_point);
            sprintf(mount_menu[mountable_volumes].unmount, "unmount %s", v->mount_point);
            sprintf(mount_menu[mountable_volumes].path, "%s", v->mount_point);
            ++mountable_volumes;
        }
    }

    for (;;) {
        for (i = 0; i < mountable_volumes; i++) {
            MountMenuEntry* e = &mount_menu[i];
            if (is_path_mounted(e->path))
                list[i] = e->unmount;
            else
                list[i] = e->mount;
        }

        list[mountable_volumes] = "mount USB storage";
        list[mountable_volumes + 1] = NULL;

        chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item < 0) // GO_BACK / REFRESH
            break;

        if (chosen_item == (mountable_volumes)) {
            show_mount_usb_storage_menu();
        } else if (chosen_item < mountable_volumes) {
            MountMenuEntry* e = &mount_menu[chosen_item];

            if (is_path_mounted(e->path)) {
                preserve_data_media(0);
                if (0 != ensure_path_unmounted(e->path))
                    LOGE("Error unmounting %s!\n", e->path);
                preserve_data_media(1);
            } else {
                if (0 != ensure_path_mounted(e->path))
                    LOGE("Error mounting %s!\n", e->path);
            }
        }
    }

    free(mount_menu);
    return chosen_item;
}
// ------ End Format and mount options

static void run_dedupe_gc() {
    char path[PATH_MAX];
    char* fmt = "%s/clockworkmod/blobs";
    char* primary_path = get_primary_storage_path();
    char** extra_paths = get_extra_storage_paths();
    int i = 0;

    sprintf(path, fmt, primary_path);
    ensure_path_mounted(primary_path);
    nandroid_dedupe_gc(path);

    if (extra_paths != NULL) {
        for (i = 0; i < get_num_extra_volumes(); i++) {
            ensure_path_mounted(extra_paths[i]);
            sprintf(path, fmt, extra_paths[i]);
            nandroid_dedupe_gc(path);
        }
    }
}

void choose_default_backup_format() {
    const char* headers[] = { "Default Backup Format", "", NULL };

    int fmt = nandroid_get_default_backup_format();

    char **list;
    char* list_tar_default[] = { "tar (default)",
                                 "dup",
                                 "tar + gzip",
                                 NULL };
    char* list_dup_default[] = { "tar",
                                 "dup (default)",
                                 "tar + gzip",
                                 NULL };
    char* list_tgz_default[] = { "tar",
                                 "dup",
                                 "tar + gzip (default)",
                                 NULL };

    if (fmt == NANDROID_BACKUP_FORMAT_DUP) {
        list = list_dup_default;
    } else if (fmt == NANDROID_BACKUP_FORMAT_TGZ) {
        list = list_tgz_default;
    } else {
        list = list_tar_default;
    }

    char path[PATH_MAX];
    sprintf(path, "%s/%s", get_primary_storage_path(), NANDROID_BACKUP_FORMAT_FILE);
    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item) {
        case 0: {
            write_string_to_file(path, "tar");
            ui_print("Default backup format set to tar.\n");
            break;
        }
        case 1: {
            write_string_to_file(path, "dup");
            ui_print("Default backup format set to dedupe.\n");
            break;
        }
        case 2: {
            write_string_to_file(path, "tgz");
            ui_print("Default backup format set to tar + gzip.\n");
            break;
        }
    }
}

static void add_nandroid_options_for_volume(char** menu, char* path, int offset) {
    char buf[100];

    sprintf(buf, "Backup to %s", path);
    menu[offset] = strdup(buf);

    sprintf(buf, "Restore from %s", path);
    menu[offset + 1] = strdup(buf);

    sprintf(buf, "Delete from %s", path);
    menu[offset + 2] = strdup(buf);

    sprintf(buf, "Custom Backup to %s", path);
    menu[offset + 3] = strdup(buf);

    sprintf(buf, "Custom Restore from %s", path);
    menu[offset + 4] = strdup(buf);
}

// number of actions added for each volume by add_nandroid_options_for_volume()
// these go on top of menu list
#define NANDROID_ACTIONS_NUM 5
// number of fixed bottom entries after volume actions
#define NANDROID_FIXED_ENTRIES 3

int show_nandroid_menu() {
    char* primary_path = get_primary_storage_path();
    char** extra_paths = get_extra_storage_paths();
    int num_extra_volumes = get_num_extra_volumes();
    int i = 0, offset = 0, chosen_item = 0;
    char* chosen_path = NULL;
    int action_entries_num = (num_extra_volumes + 1) * NANDROID_ACTIONS_NUM;
                                   // +1 for primary_path
    const char* headers[] = { "Backup and Restore", NULL };

    // (MAX_NUM_MANAGED_VOLUMES + 1) for primary_path (/sdcard)
    // + 1 for extra NULL entry
    char* list[((MAX_NUM_MANAGED_VOLUMES + 1) * NANDROID_ACTIONS_NUM) + NANDROID_FIXED_ENTRIES + 1];
    memset(list, 0, sizeof(list));

    // actions for primary_path
    add_nandroid_options_for_volume(list, primary_path, offset);
    offset += NANDROID_ACTIONS_NUM;

    // actions for voldmanaged volumes
    if (extra_paths != NULL) {
        for (i = 0; i < num_extra_volumes; i++) {
            add_nandroid_options_for_volume(list, extra_paths[i], offset);
            offset += NANDROID_ACTIONS_NUM;
        }
    }

    // fixed bottom entries
    list[offset]     = "Clone ROM to update.zip";
    list[offset + 1] = "Free Unused Backup Data";
    list[offset + 2] = "Misc Nandroid Settings";
    offset += NANDROID_FIXED_ENTRIES;

    // extra NULL for GO_BACK
    list[offset] = NULL;
    offset++;

    for (;;) {
        chosen_item = get_filtered_menu_selection(headers, list, 0, 0, offset);
        if (chosen_item < 0) // GO_BACK / REFRESH
            break;

        // fixed bottom entries
        if (chosen_item == action_entries_num) {
#ifdef PHILZ_TOUCH_RECOVERY
            custom_rom_menu();
#else
            ui_print("Unsupported in open source version!\n");
#endif
        } else if (chosen_item == (action_entries_num + 1)) {
            run_dedupe_gc();
        } else if (chosen_item == (action_entries_num + 2)) {
            misc_nandroid_menu();
        } else if (chosen_item < action_entries_num) {
            // get nandroid volume actions path
            if (chosen_item < NANDROID_ACTIONS_NUM) {
                chosen_path = primary_path;
            } else if (extra_paths != NULL) {
                chosen_path = extra_paths[(chosen_item / NANDROID_ACTIONS_NUM) - 1];
            }

            // process selected nandroid action
            int chosen_subitem = chosen_item % NANDROID_ACTIONS_NUM;
            switch (chosen_subitem) {
                case 0: {
                    char backup_path[PATH_MAX];
                    if (twrp_backup_mode.value) {
                        int fmt = nandroid_get_default_backup_format();
                        if (fmt != NANDROID_BACKUP_FORMAT_TAR && fmt != NANDROID_BACKUP_FORMAT_TGZ) {
                            LOGE("TWRP backup format must be tar(.gz)!\n");
                        } else {
                            get_twrp_backup_path(chosen_path, backup_path);
                            twrp_backup(backup_path);
                        }
                    } else {
                        get_cwm_backup_path(chosen_path, backup_path);
                        nandroid_backup(backup_path);
                    }
                    break;
                }
                case 1: {
                    if (twrp_backup_mode.value)
                        show_twrp_restore_menu(chosen_path);
                    else
                        show_nandroid_restore_menu(chosen_path);
                    break;
                }
                case 2:
                    show_nandroid_delete_menu(chosen_path);
                    break;
                case 3:
                    custom_backup_menu(chosen_path);
                    break;
                case 4:
                    custom_restore_menu(chosen_path);
                    break;
                default:
                    break;
            }
        } else {
            goto out;
        }
    }
out:
    for (i = 0; i < action_entries_num; i++)
        free(list[i]);
    return chosen_item;
}

int can_partition(const char* path) {
    if (is_data_media_volume_path(path))
        return 0;

    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
        LOGI("Can't format unknown volume: %s\n", path);
        return 0;
    }
    if (strcmp(vol->fs_type, "auto") != 0) {
        LOGI("Can't partition non-vfat: %s (%s)\n", path, vol->fs_type);
        return 0;
    }

    // do not allow partitioning of a device that isn't mmcblkX or mmcblkXp1
    // needed with new vold managed volumes and virtual device path links
    size_t vol_len;
    char *device = NULL;
    if (strstr(vol->blk_device, "/dev/block/mmcblk") != NULL) {
        device = vol->blk_device;
    } else if (vol->blk_device2 != NULL && strstr(vol->blk_device2, "/dev/block/mmcblk") != NULL) {
        device = vol->blk_device2;
    } else {
        LOGI("Can't partition non mmcblk device: %s\n", vol->blk_device);
        return 0;
    }

    vol_len = strlen(device);
    if (device[vol_len - 2] == 'p' && device[vol_len - 1] != '1') {
        LOGI("Can't partition unsafe device: %s\n", device);
        return 0;
    }

    return 1;
}

// pass in mount point as argument
void show_format_sdcard_menu(const char* path) {
    if (is_data_media_volume_path(path))
        return;

    Volume *v = volume_for_path(path);
    if (v == NULL || strcmp(v->fs_type, "auto") != 0)
        return;
    if (!fs_mgr_is_voldmanaged(v) && !can_partition(path))
        return;

    const char* headers[] = { "Format device:", path, "", NULL };

    char* list[] = {
        "default",
        "ext2",
        "ext3",
        "ext4",
        "vfat",
        "exfat",
        "ntfs",
#ifdef USE_F2FS
        "f2fs",
#endif
        NULL
    };

    int ret = -1;
    char cmd[PATH_MAX];
    int chosen_item = get_menu_selection(headers, list, 0, 0);
    if (chosen_item < 0) // REFRESH or GO_BACK
        return;
    if (!confirm_selection("Confirm formatting?", "Yes - Format device"))
        return;

    if (ensure_path_unmounted(v->mount_point) != 0)
        return;

    switch (chosen_item) {
        case 0: {
            ret = format_volume(v->mount_point);
            break;
        }
        case 1:
        case 2: {
            // workaround for new vold managed volumes that cannot be recognized by pre-built ext2/ext3 bins
            const char *device = v->blk_device2;
            if (device == NULL)
                device = v->blk_device;
            ret = format_unknown_device(device, v->mount_point, list[chosen_item]);
            break;
        }
        default: {
            if (fs_mgr_is_voldmanaged(v)) {
                ret = vold_custom_format_volume(v->mount_point, list[chosen_item], 1) == CommandOkay ? 0 : -1;
            } else if (strcmp(list[chosen_item], "vfat") == 0) {
                sprintf(cmd, "/sbin/newfs_msdos -F 32 -O android -c 8 %s", v->blk_device);
                ret = __system(cmd);
            } else if (strcmp(list[chosen_item], "exfat") == 0) {
                sprintf(cmd, "/sbin/mkfs.exfat %s", v->blk_device);
                ret = __system(cmd);
            } else if (strcmp(list[chosen_item], "ntfs") == 0) {
                sprintf(cmd, "/sbin/mkntfs -f %s", v->blk_device);
                ret = __system(cmd);
            } else if (strcmp(list[chosen_item], "ext4") == 0) {
                char *secontext = NULL;
                if (selabel_lookup(sehandle, &secontext, v->mount_point, S_IFDIR) < 0) {
                    LOGE("cannot lookup security context for %s\n", v->mount_point);
                    ret = make_ext4fs(v->blk_device, v->length, path, NULL);
                } else {
                    ret = make_ext4fs(v->blk_device, v->length, path, sehandle);
                    freecon(secontext);
                }
            }
#ifdef USE_F2FS
            else if (strcmp(list[chosen_item], "f2fs") == 0) {
                char* args[] = { "mkfs.f2fs", v->blk_device };
                ret = make_f2fs_main(2, args);
            }
#endif
            break;
        }
    }

    if (ret)
        LOGE("Could not format %s (%s)\n", path, list[chosen_item]);
    else
        ui_print("Done formatting %s (%s)\n", path, list[chosen_item]);
}

static void show_partition_sdcard_menu(const char* path) {
    if (!can_partition(path)) {
        LOGE("Can't partition device: %s\n", path);
        return;
    }

    char* ext_sizes[] = {
        "128M",
        "256M",
        "512M",
        "1024M",
        "2048M",
        "4096M",
        NULL
    };

    char* swap_sizes[] = {
        "0M",
        "32M",
        "64M",
        "128M",
        "256M",
        NULL
    };

    char* partition_types[] = {
        "ext3",
        "ext4",
        NULL
    };

    const char* ext_headers[] = { "Ext Size", "", NULL };
    const char* swap_headers[] = { "Swap Size", "", NULL };
    const char* fstype_headers[] = { "Partition Type", "", NULL };

    int ext_size = get_menu_selection(ext_headers, ext_sizes, 0, 0);
    if (ext_size < 0)
        return;

    int swap_size = get_menu_selection(swap_headers, swap_sizes, 0, 0);
    if (swap_size < 0)
        return;

    int partition_type = get_menu_selection(fstype_headers, partition_types, 0, 0);
    if (partition_type < 0) // GO_BACK / REFRESH
        return;

    char cmd[PATH_MAX];
    char sddevice[256];
    Volume *vol = volume_for_path(path);

    // can_partition() ensured either blk_device or blk_device2 has /dev/block/mmcblk format
    if (strstr(vol->blk_device, "/dev/block/mmcblk") != NULL)
        strcpy(sddevice, vol->blk_device);
    else
        strcpy(sddevice, vol->blk_device2);

    // we only want the mmcblk, not the partition
    sddevice[strlen("/dev/block/mmcblkX")] = '\0';
    setenv("SDPATH", sddevice, 1);
    sprintf(cmd, "sdparted -es %s -ss %s -efs %s -s", ext_sizes[ext_size], swap_sizes[swap_size], partition_types[partition_type]);
    ui_print("Partitioning SD Card... please wait...\n");
    if (0 == __system(cmd))
        ui_print("Done!\n");
    else
        LOGE("An error occured while partitioning your SD Card. Please see /tmp/recovery.log for more details.\n");
}

void show_advanced_power_menu() {
    const char* headers[] = { "Advanced power options", "", NULL };

    char* list[] = {
        "Reboot Recovery",
        "Reboot to Bootloader",
        "Power Off",
        NULL
    };

    char bootloader_mode[PROPERTY_VALUE_MAX];
#ifdef BOOTLOADER_CMD_ARG
    // force this extra way to use BoardConfig.mk flags
    sprintf(bootloader_mode, BOOTLOADER_CMD_ARG);
#else
    property_get("ro.bootloader.mode", bootloader_mode, "bootloader");
#endif
    if (strcmp(bootloader_mode, "download") == 0)
        list[1] = "Reboot to Download Mode";

    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item) {
        case 0:
            ui_print("Rebooting recovery...\n");
            reboot_main_system(ANDROID_RB_RESTART2, 0, "recovery");
            break;
        case 1:
            ui_print("Rebooting to %s mode...\n", bootloader_mode);
            reboot_main_system(ANDROID_RB_RESTART2, 0, bootloader_mode);
            break;
        case 2:
            ui_print("Shutting down...\n");
            reboot_main_system(ANDROID_RB_POWEROFF, 0, 0);
            break;
    }
}

#ifdef ENABLE_LOKI
#define FIXED_ADVANCED_ENTRIES 5
#else
#define FIXED_ADVANCED_ENTRIES 4
#endif

int show_advanced_menu() {
    char buf[80];
    int i = 0, j = 0, chosen_item = 0;
    /* Default number of entries if no compile-time extras are added */
    char* list[MAX_NUM_MANAGED_VOLUMES + FIXED_ADVANCED_ENTRIES + 1];

    char* primary_path = get_primary_storage_path();
    char** extra_paths = get_extra_storage_paths();
    int num_extra_volumes = get_num_extra_volumes();

    const char* headers[] = { "Advanced Menu", NULL };

    memset(list, 0, sizeof(list));

    // FIXED_ADVANCED_ENTRIES
    list[0] = "Report Error";        // 0
    list[1] = "Key Test";            // 1
    list[2] = "Show log";            // 2
    list[3] = NULL;                  // 3 (/data/media/0 toggle)
#ifdef ENABLE_LOKI
    list[4] = NULL;                  // 4
#endif

    char list_prefix[] = "Partition ";
    if (can_partition(primary_path)) {
        sprintf(buf, "%s%s", list_prefix, primary_path);
        list[FIXED_ADVANCED_ENTRIES] = strdup(buf);
        j++;
    }

    if (extra_paths != NULL) {
        for (i = 0; i < num_extra_volumes; i++) {
            if (can_partition(extra_paths[i])) {
                sprintf(buf, "%s%s", list_prefix, extra_paths[i]);
                list[FIXED_ADVANCED_ENTRIES + j] = strdup(buf);
                j++;
            }
        }
    }
    list[FIXED_ADVANCED_ENTRIES + j] = NULL;

    for (;;) {
        if (is_data_media()) {
            ensure_path_mounted("/data");
            if (use_migrated_storage())
                list[3] = "Sdcard target: /data/media/0";
            else list[3] = "Sdcard target: /data/media";
        }

#ifdef ENABLE_LOKI
        char item_loki_toggle_menu[MENU_MAX_COLS];
        int enabled = loki_support_enabled();
        if (enabled < 0) {
            list[4] = NULL;
        } else {
            if (enabled)
                ui_format_gui_menu(item_loki_toggle_menu, "Apply Loki Patch", "(x)");
            else
                ui_format_gui_menu(item_loki_toggle_menu, "Apply Loki Patch", "( )");
            list[4] = item_loki_toggle_menu;
        }
#endif

        chosen_item = get_filtered_menu_selection(headers, list, 0, 0, sizeof(list) / sizeof(char*));
        if (chosen_item < 0) // GO_BACK || REFRESH
            break;
        switch (chosen_item) {
            case 0: {
                handle_failure();
                break;
            }
            case 1: {
                ui_print("Outputting key codes.\n");
                ui_print("Go back to end debugging.\n");
                int key;
                int action;
                do {
                    key = ui_wait_key();
                    action = device_handle_key(key, 1);
                    ui_print("Key: %d\n", key);
                } while (action != GO_BACK);
                break;
            }
            case 2: {
#ifdef PHILZ_TOUCH_RECOVERY
                show_log_menu();
#else
                ui_printlogtail(24);
                ui_wait_key();
                ui_clear_key_queue();
#endif
                break;
            }
            case 3: {
                if (is_data_media()) {
                    // /data is mounted above in the for() loop: we can directly call use_migrated_storage()
                    if (use_migrated_storage()) {
                        write_string_to_file("/data/media/.cwm_force_data_media", "1");
                        ui_print("storage set to /data/media\n");
                    } else {
                        mkdir("/data/media/0", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH); 
                        delete_a_file("/data/media/.cwm_force_data_media");
                        ui_print("storage set to /data/media/0\n");
                    }
                    setup_data_media(0); // /data is mounted above in the for() loop. No need to mount/unmount on call
                    ui_print("Reboot to apply settings!\n");
                }
                break;
            }
#ifdef ENABLE_LOKI
            case 4: {
                toggle_loki_support();
                break;
            }
#endif
            default: {
                show_partition_sdcard_menu(list[chosen_item] + strlen(list_prefix));
                break;
            }
        }
    }

    for (; j > 0; --j) {
        free(list[FIXED_ADVANCED_ENTRIES + j - 1]);
    }
    return chosen_item;
}

void handle_failure() {
    if (0 != ensure_path_mounted(get_primary_storage_path()))
        return;
    mkdir("/sdcard/clockworkmod", S_IRWXU | S_IRWXG | S_IRWXO);
    __system("cp /tmp/recovery.log /sdcard/clockworkmod/philz_recovery.log");
    ui_print("/tmp/recovery.log copied to /sdcard/clockworkmod/philz_recovery.log\n");
    ui_print("Send file to Phil3759 @xda\n");
}

int is_path_mounted(const char* path) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        return 0;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted.
        return 1;
    }

    if (scan_mounted_volumes() < 0)
        return 0;

    const MountedVolume* mv = find_mounted_volume_by_mount_point(v->mount_point);
    if (mv) {
        // volume is already mounted
        return 1;
    }
    return 0;
}

int has_datadata() {
    Volume *vol = volume_for_path("/datadata");
    return vol != NULL;
}

// recovery command helper to create /etc/fstab and link /data/media path
int volume_main(int argc, char **argv) {
    load_volume_table();
    setup_data_media(1);
    return 0;
}

int verify_root_and_recovery() {
    if (!check_root_and_recovery.value)
        return 0;

    if (ensure_path_mounted("/system") != 0)
        return 0;

    int ret = 0;
    struct stat st;
    // check to see if install-recovery.sh is going to clobber recovery
    // install-recovery.sh is also used to run the su daemon on stock rom for 4.3+
    // so verify that doesn't exist...
    if (0 != lstat("/system/etc/.installed_su_daemon", &st)) {
        // check install-recovery.sh exists and is executable
        if (0 == lstat("/system/etc/install-recovery.sh", &st)) {
            if (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) {
                ui_SetShowText(true);
                if (confirm_selection("ROM may flash stock recovery on boot. Fix?", "Yes - Disable recovery flash")) {
                    __system("chmod -x /system/etc/install-recovery.sh");
                    ret = 1;
                }
            }
        }
    }

    // do not check permissions if system android version is 4.3+
    // in that case, no need to chmod 06755 as it could break root on +4.3 ROMs
    // for 4.3+, su daemon will set proper 755 permissions before app requests root
    // if no su file is found, recovery will just install su daemon on 4.3 ROMs to gain root
    // credits @Chainfire
    char value[PROPERTY_VALUE_MAX];
    int needs_suid = 1;
    read_config_file("/system/build.prop", "ro.build.version.sdk", value, "0");
    if (atoi(value) >= 18)
        needs_suid = 0;

    int exists = 0; // su exists, regular file or symlink
    int su_nums = 0; // su bin as regular file, not symlink
    if (0 == lstat("/system/bin/su", &st)) {
        exists = 1;
        if (S_ISREG(st.st_mode)) {
            su_nums += 1;
            if (needs_suid && (st.st_mode & (S_ISUID | S_ISGID)) != (S_ISUID | S_ISGID)) {
                ui_SetShowText(true);
                if (confirm_selection("Root access possibly lost. Fix?", "Yes - Fix root (/system/bin/su)")) {
                    __system("chmod 6755 /system/bin/su");
                    ret = 1;
                }
            }
        }
    }

    if (0 == lstat("/system/xbin/su", &st)) {
        exists = 1;
        if (S_ISREG(st.st_mode)) {
            su_nums += 1;
            if (needs_suid && (st.st_mode & (S_ISUID | S_ISGID)) != (S_ISUID | S_ISGID)) {
                ui_SetShowText(true);
                if (confirm_selection("Root access possibly lost. Fix?", "Yes - Fix root (/system/xbin/su)")) {
                    __system("chmod 6755 /system/xbin/su");
                    ret = 1;
                }
            }
        }
    }

    // If we have no root (exists == 0) or we have two su instances (exists == 2), prompt to properly root the device
    if (!exists || su_nums != 1) {
        ui_SetShowText(true);
        if (confirm_selection("Root access is missing/broken. Root device?", "Yes - Apply root (/system/xbin/su)")) {
            __system("/sbin/install-su.sh");
            ret = 2;
        }
    }

    ensure_path_unmounted("/system");
    return ret;
}
