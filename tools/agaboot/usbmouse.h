/* agaboot USBMOUSE, USBPAD (usbmouse.c): the Pi's own USB mouse and pad in WHDLoad games
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGABOOT_USBMOUSE_H
#define AGABOOT_USBMOUSE_H

void usbm_entry(int use);                       /* SANDBOX ON: the mouse and pad to the kernel (use 0: forgotten) */
int  usbm_tester(int on);                       /* the pad read for the Controller window's tester / no longer */
int  usbm_command(int argc, char **argv);       /* agaboot USBMOUSE ...: see usbmouse.c, it's all there */
int  usbp_command(int argc, char **argv);       /* agaboot USBPAD ...: likewise */
int  usbf_command(void);                        /* agaboot USBFIND: what the USB stack has (usbfind.c) */

#endif
