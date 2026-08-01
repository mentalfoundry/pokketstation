#ifndef POKKETSTATION_DESKTOP_RESOURCE_H
#define POKKETSTATION_DESKTOP_RESOURCE_H

#define IDI_MAINICON 100

#define IDR_MAINMENU 201
#define IDD_HWID 202
#define IDD_ADVANCED_COLORS 203
/* 204 was IDD_SHADOW_COLOR, a dialog for the sprite shadow color alone. That
   color is now one of the three IDD_ADVANCED_COLORS edits, since it only ever
   made sense picked against the scheme it sits on. */
#define IDD_REMAP_CONTROLS 205
#define IDD_CAPTURE_PROMPT 206
#define IDD_ABOUT 207

#define IDC_HWID_EDIT 1001
#define IDC_PIXEL_HEX 1002
#define IDC_PIXEL_CHOOSE 1003
#define IDC_BG_HEX 1004
#define IDC_BG_CHOOSE 1005
#define IDC_SHADOW_HEX 1006
#define IDC_SHADOW_CHOOSE 1007
/* 1008 was IDC_SHADOW_RESET, which belonged to the retired IDD_SHADOW_COLOR. */
#define IDC_CAPTURE_PROMPT_TEXT 1009
/* IDC_REMAP_LABEL_BASE and IDC_REMAP_CHANGE_BASE each reserve 9 consecutive
   IDs, one per remappable action.
   The action order is fixed: Up, Down, Left, Right, Fire, Create-Debug-Log,
   Reset, Quick-Save-State, Quick-Load-State.
   See button_scancodes in main.c for this order.
   IDC_REMAP_LABEL_BASE+i shows binding i's current key name.
   IDC_REMAP_CHANGE_BASE+i is that row's "Change..." button.
   IDC_ABOUT_LINK and IDC_ABOUT_VERSION moved up from 1025/1026, originally
   to make room for the 6th IDC_REMAP_CHANGE_BASE slot (1025).
   +0..+8 (1020-1028) still fits below IDC_ABOUT_LINK (1030), so no further
   renumbering was needed for the 3 additional rows. */
#define IDC_REMAP_LABEL_BASE 1010
#define IDC_REMAP_CHANGE_BASE 1020
#define IDC_ABOUT_LINK 1030
#define IDC_ABOUT_VERSION 1031
#define IDC_ABOUT_LICENSE_LINK 1032
/* IDD_ADVANCED_COLORS' controls, beyond the three IDC_*_HEX/IDC_*_CHOOSE
   pairs above that make up its collapsible Custom Colors group.
   IDC_SCREEN_CHOOSE is the always-visible one-color pick; it writes the same
   IDC_BG_HEX field the group's own Choose button does, and additionally
   re-derives the other two colors from it. */
#define IDC_CUSTOM_TOGGLE 1033
#define IDC_CUSTOM_GROUP 1034
#define IDC_CUSTOM_PIXEL_LABEL 1035
#define IDC_CUSTOM_SHADOW_LABEL 1036
#define IDC_COLOR_PREVIEW 1037
#define IDC_REMATCH 1038
#define IDC_CUSTOM_BG_LABEL 1039
#define IDC_SCREEN_CHOOSE 1040
#define IDC_SHADOWS_ENABLE 1041

#define ID_FILE_OPEN_BIOS 1101
#define ID_FILE_OPEN_APP 1102
#define ID_FILE_EXIT 1103
#define ID_TOOLS_EDIT_HWID 1104
#define ID_TOOLS_REMAP_CONTROLS 1115
#define ID_HELP_ABOUT 1105
#define ID_VIEW_NATIVE_SIZE 1106
#define ID_VIEW_DOUBLE_SIZE 1107
#define ID_COLORS_STANDARD 1108
#define ID_COLORS_REVERSED 1109
#define ID_COLORS_CLASSIC 1110
#define ID_COLORS_ADVANCED 1111
/* 1112-1114 were the View > Sprite Shadows submenu (Enable, Disable, Shadow
   Color...). That submenu is gone: the toggle is a checkbox in
   IDD_ADVANCED_COLORS and the color is one of its three edits. */
#define ID_FILE_RESET 1116
#define ID_FILE_QUICK_SAVE 1117
#define ID_FILE_QUICK_LOAD 1118
#define ID_IR_HOST 1119
#define ID_IR_CONNECT 1120
#define ID_IR_DISCONNECT 1121

#endif
