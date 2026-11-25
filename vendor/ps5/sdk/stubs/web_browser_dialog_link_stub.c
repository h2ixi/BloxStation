/*
 * Link-time import stubs for the PS5 in-game browser (libSceWebBrowserDialog)
 * and the common dialog library: the payload SDK ships none. Only the names
 * matter; the console binds the real functions when the title starts.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
int sceWebBrowserDialogInitialize(void) { return -1; }
int sceWebBrowserDialogOpen(const void *param) { (void)param; return -1; }
int sceWebBrowserDialogUpdateStatus(void) { return -1; }
int sceWebBrowserDialogGetStatus(void) { return -1; }
int sceWebBrowserDialogGetResult(void *result) { (void)result; return -1; }
int sceWebBrowserDialogClose(void) { return -1; }
int sceWebBrowserDialogTerminate(void) { return -1; }
