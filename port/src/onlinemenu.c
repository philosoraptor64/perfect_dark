#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "data.h"
#include "bss.h"
#include "types.h"
#include "game/menu.h"
#include "config.h"
#include "system.h"
#include "pdnet.h"

/*
 * "Play Online" (Perfect Menu): host a game (a fresh room code to tell your friend) or join one with its code,
 * through the room server in pd.ini [Net] (Server, Password; netplay.cfg's PD_NET_SERVER / PD_NET_PASSWORD
 * fill them in if empty). Connecting runs in the background; once the other player is there both games restart
 * at the title screen together (pdnet.c, pdnetOnline*).
 *
 * The game's keyboard has letters, digits, space and ?!. (17 characters at most), so the server is typed as
 * "address port" (or just the address: port 7780).
 */

#define NET_DEFAULT_PORT 7780

static char g_NetServer[128] = "";
static char g_NetPassword[128] = "";
static char g_NetLastRoom[8] = "";

PD_CONSTRUCTOR static void onlineConfigInit(void)
{
	configRegisterString("Net.Server", g_NetServer, sizeof(g_NetServer) - 1);
	configRegisterString("Net.Password", g_NetPassword, sizeof(g_NetPassword) - 1);
	configRegisterString("Net.LastRoom", g_NetLastRoom, sizeof(g_NetLastRoom) - 1);
}

static const char *netServer(void)
{
	if (g_NetServer[0]) {
		return g_NetServer;
	}
	const char *e = getenv("PD_NET_SERVER");
	return (e && *e) ? e : NULL;
}

static const char *netPassword(void)
{
	if (g_NetPassword[0]) {
		return g_NetPassword;
	}
	const char *e = getenv("PD_NET_PASSWORD");
	return (e && *e) ? e : NULL;
}

void onlineMenuSetServer(const char *server, const char *password)
{
	// invite links (iOS) and tools: remembered in pd.ini
	if (server) {
		snprintf(g_NetServer, sizeof(g_NetServer), "%s", server);
	}
	if (password) {
		snprintf(g_NetPassword, sizeof(g_NetPassword), "%s", password);
	}
}

/* --- Status: room code, waiting, errors ------------------------------------ */

static char g_OnlineStatusText[320];

static char *onlineStatusLine(struct menuitem *item)
{
	const s32 state = pdnetOnlineState();
	switch (state) {
	case PDNET_ONLINE_CONNECTING:
		if (pdnetOnlineIsHost()) {
			snprintf(g_OnlineStatusText, sizeof(g_OnlineStatusText),
				"Room code: %s\nTell your friend this code.\nWaiting for them to join...\n", pdnetOnlineRoom());
		} else {
			snprintf(g_OnlineStatusText, sizeof(g_OnlineStatusText), "Joining room %s...\n", pdnetOnlineRoom());
		}
		break;
	case PDNET_ONLINE_CONNECTED:
	case PDNET_ONLINE_ACTIVE:
		snprintf(g_OnlineStatusText, sizeof(g_OnlineStatusText), "Connected! Starting...\n");
		break;
	case PDNET_ONLINE_FAILED:
		snprintf(g_OnlineStatusText, sizeof(g_OnlineStatusText), "%s\n", pdnetOnlineError());
		break;
	default:
		snprintf(g_OnlineStatusText, sizeof(g_OnlineStatusText), "\n");
		break;
	}
	return g_OnlineStatusText;
}

static char *onlineStatusButton(struct menuitem *item)
{
	return pdnetOnlineState() == PDNET_ONLINE_FAILED ? "OK\n" : "Cancel\n";
}

static MenuItemHandlerResult menuhandlerOnlineStatusButton(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		pdnetOnlineCancel();
		pdnetOnlineDismiss();
	}
	return 0;
}

static MenuDialogHandlerResult menudialogOnlineStatus(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_CLOSE) {
		// leaving the dialog (Back) stops a connection that hasn't been made yet
		pdnetOnlineCancel();
		pdnetOnlineDismiss();
	}
	return 0;
}

struct menuitem g_OnlineStatusMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)&onlineStatusLine,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		(uintptr_t)&onlineStatusButton,
		0,
		menuhandlerOnlineStatusButton,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_OnlineStatusMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Online",
	g_OnlineStatusMenuItems,
	menudialogOnlineStatus,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/* --- Join: type the room code ---------------------------------------------- */

static MenuItemHandlerResult menuhandlerOnlineRoomCode(s32 operation, struct menuitem *item, union handlerdata *data)
{
	char *code = data->keyboard.string;

	switch (operation) {
	case MENUOP_GETTEXT:
		snprintf(code, MPSETUP_MAXNAME + 1, "%s", g_NetLastRoom);
		break;
	case MENUOP_SETTEXT:
		snprintf(g_NetLastRoom, sizeof(g_NetLastRoom), "%.4s", code);
		break;
	case MENUOP_SET:
		snprintf(g_NetLastRoom, sizeof(g_NetLastRoom), "%.4s", code);
		pdnetOnlineJoin(netServer(), netPassword(), g_NetLastRoom);
		menuPopDialog();
		menuPushDialog(&g_OnlineStatusMenuDialog);
		break;
	}
	return 0;
}

struct menuitem g_OnlineJoinMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Room code from the host:\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_KEYBOARD,
		4, // max length
		0,
		0,
		0,
		menuhandlerOnlineRoomCode,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_OnlineJoinMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Join a Game",
	g_OnlineJoinMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_DISABLEBANNER,
	NULL,
};

/* --- Server address ---------------------------------------------------------- */

static MenuItemHandlerResult menuhandlerOnlineServer(s32 operation, struct menuitem *item, union handlerdata *data)
{
	char *text = data->keyboard.string;

	switch (operation) {
	case MENUOP_GETTEXT: {
		// shown as "address port"
		const char *cur = netServer() ? netServer() : "";
		snprintf(text, MPSETUP_MAXNAME + 1, "%s", cur);
		char *colon = strrchr(text, ':');
		if (colon) *colon = ' ';
		break;
	}
	case MENUOP_SET: {
		char addr[64] = "";
		s32 port = NET_DEFAULT_PORT;
		if (sscanf(text, "%63s %d", addr, &port) >= 1 && addr[0]) {
			snprintf(g_NetServer, sizeof(g_NetServer), "%s:%d", addr, port);
			for (char *c = g_NetServer; *c; c++) {
				if (*c >= 'A' && *c <= 'Z') *c += 'a' - 'A'; // host names
			}
		}
		menuPopDialog();
		break;
	}
	}
	return 0;
}

struct menuitem g_OnlineServerMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Room server: address, space, port\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_KEYBOARD,
		MPSETUP_MAXNAME, // max length
		0,
		0,
		0,
		menuhandlerOnlineServer,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_OnlineServerMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Room Server",
	g_OnlineServerMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_DISABLEBANNER,
	NULL,
};

/* --- Play Online -------------------------------------------------------------- */

static char g_OnlineServerText[200];

static char *onlineServerLine(struct menuitem *item)
{
	if (netServer()) {
		snprintf(g_OnlineServerText, sizeof(g_OnlineServerText), "Server: %s%s\n", netServer(),
			netPassword() ? "" : " (no password)");
	} else {
		snprintf(g_OnlineServerText, sizeof(g_OnlineServerText), "No room server set yet.\n");
	}
	return g_OnlineServerText;
}

static MenuItemHandlerResult menuhandlerOnlineHost(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return netServer() == NULL;
	}
	if (operation == MENUOP_SET) {
		pdnetOnlineHost(netServer(), netPassword());
		menuPushDialog(&g_OnlineStatusMenuDialog);
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerOnlineJoin(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return netServer() == NULL;
	}
	if (operation == MENUOP_SET) {
		menuPushDialog(&g_OnlineJoinMenuDialog);
	}
	return 0;
}

struct menuitem g_OnlineMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)&onlineServerLine,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Host a Game\n",
		0,
		menuhandlerOnlineHost,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Join a Game\n",
		0,
		menuhandlerOnlineJoin,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Room Server...\n",
		0,
		(void *)&g_OnlineServerMenuDialog,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_OnlineMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Play Online",
	g_OnlineMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/* Perfect Menu item: hidden during an online game (both machines run that menu, so both hide it) */
MenuItemHandlerResult menuhandlerMainMenuPlayOnline(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return pdnetActive();
	}
	if (operation == MENUOP_SET) {
		menuPushDialog(&g_OnlineMenuDialog);
	}
	return 0;
}

/* test aid PD_NET_TEST_MENU=tick: opens Play Online on top of the menu shown at that tick (screenshots) */
void onlineMenuTestTick(s32 tick)
{
	static s32 at = -2;
	if (at == -2) {
		const char *e = getenv("PD_NET_TEST_MENU");
		at = (e && *e) ? atoi(e) : -1;
	}
	if (at >= 0 && tick >= at) {
		at = -1;
		sysLogPrintf(LOG_NOTE, "online menu: test open at tick %d (menu root %d)", tick, g_MenuData.root);
		menuPushDialog(&g_OnlineMenuDialog);
	}
}
