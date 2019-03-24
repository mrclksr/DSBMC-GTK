/*-
 * Copyright (c) 2016 Marcel Kaiser. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <err.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <paths.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include "dsbcfg/dsbcfg.h"
#include "gtk-helper/gtk-helper.h"

#define PROGRAM		  "dsbmc"
#define PATH_CONFIG	  "config"
#define TITLE		  "DSBMC"
#define PATH_BOOKMARK	  ".gtk-bookmarks"
#define PATH_DSBMD_SOCKET "/var/run/dsbmd.socket"
#define PATH_LOCK	  ".dsbmc.lock"
#define CMDQSZ		  16

#define LABEL_WIDTH	  16
#define CDR_MAXSPEED	  52
#define ICON_LOOKUP_FLAGS \
	(GTK_ICON_LOOKUP_USE_BUILTIN | GTK_ICON_LOOKUP_GENERIC_FALLBACK)

#define MARKUP_BOLD	  "<span font_weight=\"bold\" "			   \
			  "underline=\"single\">%s</span>"
#define MARKUP_NORMAL	  "<span font_weight=\"normal\">%s</span>"

#define EJECT_BUSY_MSG							   \
	"The device is busy. Try to terminate all applications which are " \
	"currently accessing the device or files on the mounted "	   \
	"filesystem.\n\n<span font_weight=\"bold\">Shall I force "	   \
	"ejecting the media?</span>"

#define UNMOUNT_BUSY_MSG						   \
	"The device is busy. Try to terminate all applications which are " \
	"currently accessing the device or files on the mounted "	   \
	"filesystem.\n\n<span font_weight=\"bold\">Shall I force "	   \
	"unmounting the device?</span>"

#define SETTINGS_MENU_INFO_MSG						   \
	"<span font=\"monospace\" font_weight=\"bold\">%%d</span> "	   \
	"will be replaced by the device name.\n"			   \
	"<span font=\"monospace\" font_weight=\"bold\">%%m</span> "	   \
	"will be replaced by the mount point."

#define BUSYWIN_MSG _("Please wait ... ")

typedef struct icon_s	 icon_t;
typedef struct drive_s	 drive_t;
typedef struct ctxmenu_s ctxmenu_t;

static int	  process_event(char *);
static int	  parse_dsbmdevent(char *);
static int	  create_mddev(const char *);
static char	  *readln(bool);
static FILE	  *uconnect(const char *);
static void	  usage(void);
static void	  cleanup(int);
static void	  catch_child(int);
static void	  sndcmd(void (*re)(icon_t *), icon_t *,
			const char *, ...);
static void	  exec_cmd(const char *, drive_t *);
static void	  create_mainwin(void);
static void	  hide_win(GtkWidget *);
static void	  tray_click(GtkStatusIcon *, gpointer);
static void	  popup_tray_ctxmenu(GtkStatusIcon *, guint, guint, gpointer);
static void	  settings_menu(void);
static void	  update_icons(void);
static void	  set_mounted(icon_t *, bool);
static void	  add_bookmark(const char *);
static void	  del_bookmark(const char *);
static void	  create_icon_list(void);
static void	  load_pixbufs(void);
static void	  del_drive(const char *);
static void	  del_icon(const char *);
static void	  cb_mount(GtkWidget *, gpointer);
static void	  cb_unmount(GtkWidget *, gpointer);
static void	  cb_eject(GtkWidget *, gpointer);
static void	  cb_speed(GtkWidget *, gpointer);
static void	  cb_open(GtkWidget *, gpointer);
static void	  cb_play(GtkWidget *, gpointer);
static void	  cb_size(GtkWidget *, gpointer);
static void	  cb_cb(GtkWidget *, gpointer);
static void	  process_mount_reply(icon_t *);
static void	  process_unmount_reply(icon_t *);
static void	  process_open_reply(icon_t *);
static void	  process_size_reply(icon_t *);
static void	  process_speed_reply(icon_t *);
static void	  process_eject_reply(icon_t *);
static void	  call_reply_function(void);
static void	  busywin(const char *msg, bool show);
static icon_t	  *add_icon(drive_t *);
static drive_t	  *add_drive(drive_t *);
static drive_t	  *lookupdrv(const char *);
static drive_t	  *lookupdrv_from_mnt(const char *);
static gboolean	  window_state_event(GtkWidget *, GdkEvent *, gpointer);
static gboolean	  readevent(GIOChannel *, GIOCondition, gpointer);
static gboolean	  icon_clicked(GtkWidget *, GdkEvent *, gpointer);
static GdkPixbuf  *lookup_pixbuf(const char *);
static ctxmenu_t  *create_ctxmenu(icon_t *);
static const char *errmsg(int);
static GtkListStore  *create_icontbl(GtkListStore *);

struct drive_s {
	u_int cmds;		/* Supported commands. */
#define DRVCMD_MOUNT	(1 << 0x00)
#define DRVCMD_UNMOUNT	(1 << 0x01)
#define DRVCMD_EJECT	(1 << 0x02)
#define DRVCMD_PLAY	(1 << 0x03)
#define DRVCMD_OPEN	(1 << 0x04)
#define DRVCMD_SPEED	(1 << 0x05)
#define DRVCMD_HIDE	(1 << 0x06)
	char  type;
#define DSKTYPE_HDD	0x01
#define DSKTYPE_USBDISK	0x02
#define DSKTYPE_DATACD	0x03
#define	DSKTYPE_AUDIOCD	0x04
#define	DSKTYPE_RAWCD	0x05
#define	DSKTYPE_DVD	0x06
#define	DSKTYPE_VCD	0x07
#define	DSKTYPE_SVCD	0x08
#define	DSKTYPE_FLOPPY	0x09
#define DSKTYPE_MMC	0x0a
#define DSKTYPE_PTP	0x0b
#define DSKTYPE_MTP	0x0c
	int   speed;
	char  *dev;		/* Device name */
	char  *volid;		/* Volume ID */
	char  *mntpt;		/* Mount point */
	char  *fsname;		/* Filesystem name */
	bool  mounted;		/* Whether drive is mounted. */
};

struct dsbmdevent_s {
	char	 type;		/* Event type. */
#define EVENT_SUCCESS_MSG 'O'
#define EVENT_WARNING_MSG 'W'
#define EVENT_ERROR_MSG	  'E'
#define EVENT_MOUNT	  'M'
#define EVENT_UNMOUNT	  'U'
#define EVENT_SHUTDOWN	  'S'
#define EVENT_SPEED	  'V'
#define EVENT_ADD_DEVICE  '+'
#define EVENT_DEL_DEVICE  '-'
	char	 *command;	/* In case of a reply, the executed command. */
	int	 mntcmderr;	/* Return code of external mount command. */
	int	 code;		/* The error code */
	uint64_t mediasize;	/* For "size" command. */
	uint64_t free;		/* 	 ""	       */
	uint64_t used;		/* 	 ""	       */
#define ERR_ALREADY_MOUNTED     ((1 << 8) + 0x01)
#define ERR_PERMISSION_DENIED   ((1 << 8) + 0x02)
#define ERR_NOT_MOUNTED         ((1 << 8) + 0x03)
#define ERR_DEVICE_BUSY         ((1 << 8) + 0x04)
#define ERR_NO_SUCH_DEVICE      ((1 << 8) + 0x05)
#define ERR_MAX_CONN_REACHED    ((1 << 8) + 0x06)
#define ERR_NOT_EJECTABLE       ((1 << 8) + 0x07)
#define ERR_UNKNOWN_COMMAND     ((1 << 8) + 0x08)
#define ERR_UNKNOWN_OPTION      ((1 << 8) + 0x09)
#define ERR_SYNTAX_ERROR        ((1 << 8) + 0x0a)
#define ERR_NO_MEDIA            ((1 << 8) + 0x0b)
#define ERR_UNKNOWN_FILESYSTEM  ((1 << 8) + 0x0c)
#define ERR_UNKNOWN_ERROR       ((1 << 8) + 0x0d)
#define ERR_MNTCMD_FAILED       ((1 << 8) + 0x0e)
#define ERR_INVALID_ARGUMENT	((1 << 8) + 0x0f)
#define ERR_STRING_TOO_LONG	((1 << 8) + 0x10)
#define ERR_BAD_STRING		((1 << 8) + 0x11)
#define ERR_TIMEOUT		((1 << 8) + 0x12)
#define ERR_NOT_A_FILE		((1 << 8) + 0x13)
	drive_t drvinfo;	/* For Add/delete/mount/unmount message. */
} dsbmdevent;

typedef union val_u val_t;
struct dsbmdkeyword_s {
	const char *key;
	u_char	   type;
#define	KWTYPE_CHAR	0x01
#define	KWTYPE_STRING	0x02
#define	KWTYPE_COMMANDS	0x03
#define KWTYPE_INTEGER 	0x04
#define KWTYPE_UINT64	0x05
#define	KWTYPE_DSKTYPE	0x06
	union val_u {
		int	 *integer;
		char	 *character;
		char	 **string;
		uint64_t *uint64;
	} val;
} dsbmdkeywords[] = {
	{ "+",		KWTYPE_CHAR,	 (val_t)&dsbmdevent.type	   },
	{ "-",		KWTYPE_CHAR,	 (val_t)&dsbmdevent.type	   },
	{ "O",		KWTYPE_CHAR,	 (val_t)&dsbmdevent.type	   },
	{ "E",		KWTYPE_CHAR,	 (val_t)&dsbmdevent.type	   },
	{ "M",		KWTYPE_CHAR,	 (val_t)&dsbmdevent.type	   },
	{ "U",		KWTYPE_CHAR,	 (val_t)&dsbmdevent.type	   },
	{ "V",		KWTYPE_CHAR,	 (val_t)&dsbmdevent.type	   },
	{ "S",		KWTYPE_CHAR,	 (val_t)&dsbmdevent.type	   },
	{ "command=",	KWTYPE_STRING,	 (val_t)&dsbmdevent.command	   },
	{ "dev=",	KWTYPE_STRING,	 (val_t)&dsbmdevent.drvinfo.dev    },
	{ "fs=",	KWTYPE_STRING,	 (val_t)&dsbmdevent.drvinfo.fsname },
	{ "volid=",	KWTYPE_STRING,	 (val_t)&dsbmdevent.drvinfo.volid  },
	{ "mntpt=",	KWTYPE_STRING,	 (val_t)&dsbmdevent.drvinfo.mntpt  },
	{ "type=",	KWTYPE_DSKTYPE,	 (val_t)&dsbmdevent.drvinfo.type   },
	{ "speed=",	KWTYPE_INTEGER,	 (val_t)&dsbmdevent.drvinfo.speed  },
	{ "code=",	KWTYPE_INTEGER,	 (val_t)&dsbmdevent.code	   },
	{ "cmds=",	KWTYPE_COMMANDS, (val_t)(char *)0		   },
	{ "mntcmderr=",	KWTYPE_INTEGER,	 (val_t)&dsbmdevent.mntcmderr	   },
	{ "mediasize=", KWTYPE_UINT64,	 (val_t)&dsbmdevent.mediasize	   },
	{ "used=",	KWTYPE_UINT64,	 (val_t)&dsbmdevent.used	   },
	{ "free=",	KWTYPE_UINT64,	 (val_t)&dsbmdevent.free	   }
};
#define NKEYWORDS (sizeof(dsbmdkeywords) / sizeof(struct dsbmdkeyword_s))

/*
 * Struct to translate DSBMD error codes into sentences.
 */
static struct error_s {
	int	   error;
	const char *msg;
} errorcodes[] = {
	{ ERR_ALREADY_MOUNTED,	  "Device already mounted"		   },
	{ ERR_PERMISSION_DENIED,  "Permission denied"			   },
	{ ERR_NOT_MOUNTED,	  "Device not mounted"			   },
	{ ERR_DEVICE_BUSY,	  "Device busy"				   },
	{ ERR_NO_SUCH_DEVICE,	  "No such device"			   },
	{ ERR_MAX_CONN_REACHED,   "Maximal number of connections reached"  },
	{ ERR_NOT_EJECTABLE,	  "Media not ejectable"			   },
	{ ERR_UNKNOWN_COMMAND,	  "Unknow command"			   },
	{ ERR_UNKNOWN_OPTION,	  "Unknow option"			   },
	{ ERR_SYNTAX_ERROR,	  "Syntax error"			   },
	{ ERR_NO_MEDIA,		  "No media in drive"			   },
	{ ERR_UNKNOWN_FILESYSTEM, "Unknown filesystem"			   },
	{ ERR_UNKNOWN_ERROR,	  "Unknown error"			   },
	{ ERR_MNTCMD_FAILED,	  "Mouting failed"			   },
	{ ERR_STRING_TOO_LONG,	  "Command string too long"		   },
	{ ERR_BAD_STRING,	  "Invalid command string"		   },
	{ ERR_TIMEOUT,		  "Timeout"				   },
	{ ERR_NOT_A_FILE,	  "Not a regular file"			   }
};
#define NERRCODES (sizeof(errorcodes) / sizeof(struct error_s))

#define NPROCS 16
static struct process_s {
	int   error;
#define EXIT_ENOENT   127	  /* The command could not be found. */
#define EXIT_EXEC_ERR 255	  /* Execution of the shell failed. */
	int   saved_errno;	  /* errno set by execl(). */
	char  *cmdstr;		  /* The command which was executed. */
	pid_t pid;		  /* The command's PID. */
} proctbl[NPROCS];

/*
 * Struct to load pixbufs for device icons and context menu items.
 */
static struct pixbuftbl_s {
	const char *id;
	const int  iconsize;
#define ICON_SIZE_ICON 46
#define ICON_SIZE_MENU 16
	GdkPixbuf  *icon;	/* Icons for menu and devices. */
	const char *name[4];	/* Icon name with alternatives. */
} pixbuftbl[] = {
	{ "MTP",     ICON_SIZE_ICON, NULL, { "multimedia-player",
					     "drive-harddisk-usb",   NULL } },
	{ "PTP",     ICON_SIZE_ICON, NULL, { "camera-photo",
					     "drive-harddisk-usb",   NULL } },
	{ "DVD",     ICON_SIZE_ICON, NULL, { "media-optical-dvd",
					     "drive-optical",        NULL } },
	{ "DATACD",  ICON_SIZE_ICON, NULL, { "media-optical-cd",
					     "drive-optical",	     NULL } },
	{ "USBDISK", ICON_SIZE_ICON, NULL, { "drive-harddisk-usb",
					     "drive-removable-media",
					     "drive-harddisk",	     NULL } },
	{ "RAWCD",   ICON_SIZE_ICON, NULL, { "media-optical-cd",
					     "drive-optical",	     NULL } },
	{ "AUDIOCD", ICON_SIZE_ICON, NULL, { "media-optical-audio",
					     "drive-optical",	     NULL } },
	{ "VCD",     ICON_SIZE_ICON, NULL, { "media-optical-cd",
					     "drive-optical",	     NULL } },
	{ "SVCD",    ICON_SIZE_ICON, NULL, { "media-optical-cd",
					     "drive-optical",	     NULL } },
	{ "HDD",     ICON_SIZE_ICON, NULL, { "drive-harddisk",
					     "harddrive",	     NULL } },
	{ "MMC",     ICON_SIZE_ICON, NULL, { "media-flash-sd-mmc",
					     "media-flash",	     NULL } },
	{ "FLOPPY",  ICON_SIZE_ICON, NULL, { "media-floppy",	     NULL } },
	{ "mounted", ICON_SIZE_ICON, NULL, { "folder",		     NULL } },
	{ "dvd",     ICON_SIZE_MENU, NULL, { "media-optical-dvd",
					     "drive-optical",	     NULL } },
	{ "cd",	     ICON_SIZE_MENU, NULL, { "media-optical-cd",
					     "drive-optical",	     NULL } },
	{ "cdda",    ICON_SIZE_MENU, NULL, { "media-optical-audio",
					     "drive-optical",	     NULL } },
	{ "vcd",     ICON_SIZE_MENU, NULL, { "media-optical-cd",
					     "drive-optical",	     NULL } },
	{ "svcd",    ICON_SIZE_MENU, NULL, { "media-optical-cd",
					     "drive-optical",	     NULL } },
	{ "eject",   ICON_SIZE_MENU, NULL, { "media-eject",	     NULL } },
	{ "play",    ICON_SIZE_MENU, NULL, { "media-playback-start", NULL } },
	{ "open",    ICON_SIZE_MENU, NULL, { "document-open",	     NULL } },
	{ "mount",   ICON_SIZE_MENU, NULL, { "go-up",		     NULL } },
	{ "unmount", ICON_SIZE_MENU, NULL, { "go-down",		     NULL } },
	{ "hide",    ICON_SIZE_MENU, NULL, { "list-remove",
					     "emblem-unreadable",    NULL } }
};
#define PIXBUFTBLSZ (sizeof(pixbuftbl) / sizeof(struct pixbuftbl_s))

/*
 * Struct to create context menus for the device icons.  The order of the
 * following entries represent the order of the menu items in the context
 * menu.
 */
static struct menu_commands_s {
	const u_int cmd;
	const char  *name;		   /* Menu Item name. */
	void (*cb)(GtkWidget *, gpointer); /* Callback function. */
} menucmds[] = {
	{ DRVCMD_OPEN,	  "_Open",		   &cb_open    },
	{ DRVCMD_PLAY,	  "_Play",		   &cb_play    },
	{ DRVCMD_MOUNT,	  "_Mount drive",	   &cb_mount   },
	{ DRVCMD_UNMOUNT, "_Unmount drive",	   &cb_unmount },
	{ DRVCMD_SPEED,	  "_Set max. CDROM speed", &cb_speed   },
	{ DRVCMD_EJECT,	  "_Eject media",	   &cb_eject   }
};
#define NMENUCMDS (sizeof(menucmds) / sizeof(struct menu_commands_s))

/*
 * Struct to assign disk type strings to disktype IDs and pixbufs.
 */
struct disktypetbl_s {
	const char   *name;
	const u_char type;
	GdkPixbuf    *pix_normal;
	GdkPixbuf    *pix_mounted;
} disktypetbl[] = {
	{ "AUDIOCD", DSKTYPE_AUDIOCD, NULL, NULL },
	{ "DATACD",  DSKTYPE_DATACD,  NULL, NULL },
	{ "RAWCD",   DSKTYPE_RAWCD,   NULL, NULL },
	{ "USBDISK", DSKTYPE_USBDISK, NULL, NULL },
	{ "FLOPPY",  DSKTYPE_FLOPPY,  NULL, NULL },
	{ "DVD",     DSKTYPE_DVD,     NULL, NULL },
	{ "VCD",     DSKTYPE_VCD,     NULL, NULL },
	{ "SVCD",    DSKTYPE_SVCD,    NULL, NULL },
	{ "HDD",     DSKTYPE_HDD,     NULL, NULL },
	{ "MMC",     DSKTYPE_MMC,     NULL, NULL },
	{ "PTP",     DSKTYPE_PTP,     NULL, NULL },
	{ "MTP",     DSKTYPE_MTP,     NULL, NULL }
};
#define NDSKTYPES (sizeof(disktypetbl) / sizeof(struct disktypetbl_s))

/*
 * Struct to assign command names to command flags/IDs and a pixbuf.
 */
struct cmdtbl_s {
	const char  *name;
	const u_int cmd;
	GdkPixbuf   *pix;
} cmdtbl[] = {
	{ "open",    DRVCMD_OPEN,    NULL },
	{ "play",    DRVCMD_PLAY,    NULL },
	{ "mount",   DRVCMD_MOUNT,   NULL },
	{ "unmount", DRVCMD_UNMOUNT, NULL },
	{ "eject",   DRVCMD_EJECT,   NULL },
	{ "speed",   DRVCMD_SPEED,   NULL }
};
#define NCMDS (sizeof(cmdtbl) / sizeof(struct cmdtbl_s))

enum MenuItems {
	MENU_ITEM_MOUNT,
	MENU_ITEM_UNMOUNT,
	NMENU_ITEMS
};
struct ctxmenu_s {
	GtkWidget *menu;
	GtkWidget *menuitems[NMENU_ITEMS];
};

/*
 * Struct to represent a device icon.
 */
struct icon_s {
	drive_t	    *drvp;
	ctxmenu_t   *ctxmenu;	  /* Context menu. */
	GtkWidget   *label;	  /* A pointer to the icon's label. */
	GdkPixbuf   *pix_mounted; /* Icon pixbuf to use when mounted */
	GdkPixbuf   *pix_normal;  /* Icon pixbuf to use when not mounted. */
	GdkPixbuf   *pixbuf;	  /* Current icon pixbuf. */
	GtkTreeIter iter;
};
enum {
	COL_NAME, COL_PIXBUF, COL_ICON, NUM_COLS
};

/*
 * Struct to hold all the main window's variables together.
 */
static struct mainwin_s {
	int	       *posx;	  /* Window's X-position */
	int	       *posy;	  /* Window's Y-position */
	int	       *width;	  /* Window's width */
	int	       *height;	  /* Window's height */
	GtkWidget      *icon_view;
	GtkWidget      *statusbar;
	GtkWindow      *win;	  /* Main window. */
	GtkListStore   *store;	  /* Device icon grid. */
	GtkStatusIcon  *tray_icon;
	GdkWindowState win_state; /* Visible, hidden, etc. */
} mainwin;

/*
 * Struct to create the settings menu. 
 */
enum {
	SETTINGS_FM, SETTINGS_DVD, SETTINGS_VCD, SETTINGS_SVCD,
	SETTINGS_CDDA, SETTINGS_NCMDS
};
static struct settingsmenu_s {
	char ***ignore_list;
	struct settings_cmd_s {
		const char *action;
		char	   **cmdstr;
		const char *iconid;
		bool	   *autoplay;
	} cmds[SETTINGS_NCMDS];
} settingsmenu = {
	.cmds = {
		{ "Filemanager: ",	   NULL, "open", NULL },
		{ "Play DVDs with: ",	   NULL, "dvd",  NULL },
		{ "Play VCDs with: ",	   NULL, "vcd",  NULL },
		{ "Play SVCDs with: ",	   NULL, "svcd", NULL },
		{ "Play Audio CDs with: ", NULL, "cdda", NULL }
	}
};

/*
 * Struct to define a command queue. It makes sure commands
 * will be processed in the correct order. A command will only
 * be executed if the previous was processed.
 */
struct command_s {
	char cmd[128];		/* Command string to send to DSBMD */
	void (*re)(icon_t *);	/* Function to call on DSBMD reply */
	icon_t *icon;		/* icon/device command  refers to. */
} cmdq[CMDQSZ];


/*
 * Definition of config file variables and their default values.
 */
#define DEFAULT_WIDTH	     300
#define DEFAULT_HEIGHT	     300
#define DFLT_CMD_PLAY_CDDA   DSBCFG_VAL("deadbeef all.cda")
#define DFLT_CMD_PLAY_DVD    DSBCFG_VAL("vlc dvd://%d")
#define DFLT_CMD_PLAY_VCD    DSBCFG_VAL("vlc vcd://%d")
#define DFLT_CMD_PLAY_SVCD   DSBCFG_VAL("vlc vcd://%d")
#define DFLT_CMD_FILEMANAGER DSBCFG_VAL("thunar \"%m\"")

enum {
	CFG_PLAY_CDDA, CFG_PLAY_DVD, CFG_PLAY_VCD, CFG_PLAY_SVCD,
	CFG_FILEMANAGER, CFG_DVD_AUTO, CFG_VCD_AUTO, CFG_SVCD_AUTO,
	CFG_CDDA_AUTO, CFG_WIDTH, CFG_HEIGHT, CFG_POS_X, CFG_POS_Y,
	CFG_HIDE, CFG_NVARS
};

static dsbcfg_vardef_t vardefs[] = {
  { "win_pos_x",   DSBCFG_VAR_INTEGER, CFG_POS_X,       DSBCFG_VAL(0)	     },
  { "win_pos_y",   DSBCFG_VAR_INTEGER, CFG_POS_Y,       DSBCFG_VAL(0)	     },
  { "win_width",   DSBCFG_VAR_INTEGER, CFG_WIDTH,       DSBCFG_VAL(346)	     },
  { "win_height",  DSBCFG_VAR_INTEGER, CFG_HEIGHT,      DSBCFG_VAL(408)      },
  { "filemanager", DSBCFG_VAR_STRING,  CFG_FILEMANAGER, DFLT_CMD_FILEMANAGER },
  { "play_cdda",   DSBCFG_VAR_STRING,  CFG_PLAY_CDDA,   DFLT_CMD_PLAY_CDDA   },
  { "play_dvd",    DSBCFG_VAR_STRING,  CFG_PLAY_DVD,    DFLT_CMD_PLAY_DVD    },
  { "play_vcd",    DSBCFG_VAR_STRING,  CFG_PLAY_VCD,    DFLT_CMD_PLAY_VCD    },
  { "play_svcd",   DSBCFG_VAR_STRING,  CFG_PLAY_SVCD,   DFLT_CMD_PLAY_SVCD   },
  { "dvd_auto",    DSBCFG_VAR_BOOLEAN, CFG_DVD_AUTO,    DSBCFG_VAL(false)    },
  { "vcd_auto",    DSBCFG_VAR_BOOLEAN, CFG_VCD_AUTO,    DSBCFG_VAL(false)    },
  { "svcd_auto",   DSBCFG_VAR_BOOLEAN, CFG_SVCD_AUTO,   DSBCFG_VAL(false)    },
  { "cdda_auto",   DSBCFG_VAR_BOOLEAN, CFG_CDDA_AUTO,   DSBCFG_VAL(false)    },
  { "ignore",	   DSBCFG_VAR_STRINGS, CFG_HIDE,	DSBCFG_VAL((char **)NULL)     }
};

static int	cmdqlen = 0;	  /* # of commands in command queue. */
static int	cmdqidx = 0;	  /* # current command to exec. in queue */
static int      nicons  = 0;	  /* # of device icons. */
static int      ndrives = 0;	  /* # of drives. */
static FILE     *sock;		  /* Socket connected to dsbmd. */
static icon_t   **icons  = NULL;  /* List of device icons. */ 
static drive_t  **drives = NULL;  /* List of drives. */
static dsbcfg_t *cfg	 = NULL;

static void
sndcmd(void (*re)(icon_t *), icon_t *icon, const char *cmd, ...)
{
	va_list ap;
	
	if (cmdqlen == CMDQSZ)
		/* Command queue is full. Just ignore further commands. */
		return;
	va_start(ap, cmd);
	(void)vsnprintf(cmdq[cmdqlen].cmd, sizeof(cmdq[cmdqlen].cmd), cmd, ap);
	cmdq[cmdqlen].re   = re;
	cmdq[cmdqlen].icon = icon;

	if (cmdqlen++ == 0) {
		/* First command in queue. */
		cmdqidx = 0;
		(void)fputs(cmdq[0].cmd, sock);
	}
}

int
main(int argc, char *argv[])
{
	int			  ch, i, lockfd;
	gint	      iotag;
	char	      *p, *path, path_lock[PATH_MAX];
	sigset_t      sigmask;
	GIOChannel    *ioc;
	struct passwd *pw;

#ifdef WITH_GETTEXT
	(void)setlocale(LC_ALL, "");
	(void)bindtextdomain(PROGRAM, PATH_LOCALE);
	(void)textdomain(PROGRAM);
#endif
	gtk_init(&argc, &argv);

	mainwin.win_state = GDK_WINDOW_STATE_ABOVE;
	while ((ch = getopt(argc, argv, "ih")) != -1) {
		switch (ch) {
		case 'i':
			/* Start as tray icon. */
			mainwin.win_state = GDK_WINDOW_STATE_WITHDRAWN;
			break;
		case '?':
		case 'h':
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if ((pw = getpwuid(getuid())) == NULL)
		xerr(NULL, EXIT_FAILURE, "getpwuid()");
        /* Check if another instance is already running. */
	(void)snprintf(path_lock, sizeof(path_lock), "%s/%s", pw->pw_dir,
	    PATH_LOCK);
	endpwent();
	if ((lockfd = open(path_lock, O_WRONLY | O_CREAT, 0600)) == -1)
		xerr(NULL, EXIT_FAILURE, "open(%s)", path_lock);
	if (flock(lockfd, LOCK_EX | LOCK_NB) == -1) {
		if (errno == EWOULDBLOCK) {
			while (argc--)
				(void)create_mddev(argv[argc]);
			exit(EXIT_SUCCESS);
		}
		xerr(NULL, EXIT_FAILURE, "flock(%s)", path_lock);
	}
	while (argc--)
		(void)create_mddev(argv[argc]);
	if (sock != NULL)
		(void)fclose(sock);

	cfg = dsbcfg_read(PROGRAM, PATH_CONFIG, vardefs, CFG_NVARS);
	if (cfg == NULL && errno == ENOENT) {
		cfg = dsbcfg_new(NULL, vardefs, CFG_NVARS);
		if (cfg == NULL)
			xerrx(NULL, EXIT_FAILURE, "%s", dsbcfg_strerror());
	} else if (cfg == NULL)
		xerrx(NULL, EXIT_FAILURE, "%s", dsbcfg_strerror());

	mainwin.posx   = &dsbcfg_getval(cfg, CFG_POS_X).integer;
	mainwin.posy   = &dsbcfg_getval(cfg, CFG_POS_Y).integer;
	mainwin.width  = &dsbcfg_getval(cfg, CFG_WIDTH).integer;
	mainwin.height = &dsbcfg_getval(cfg, CFG_HEIGHT).integer;

	settingsmenu.cmds[SETTINGS_FM].cmdstr =
	    &dsbcfg_getval(cfg, CFG_FILEMANAGER).string;

	settingsmenu.cmds[SETTINGS_DVD].autoplay =
	    (bool *)&dsbcfg_getval(cfg, CFG_DVD_AUTO).boolean;
	settingsmenu.cmds[SETTINGS_DVD].cmdstr = 
	    &dsbcfg_getval(cfg, CFG_PLAY_DVD).string;

	settingsmenu.cmds[SETTINGS_VCD].autoplay =
	    (bool *)&dsbcfg_getval(cfg, CFG_VCD_AUTO).boolean;
	settingsmenu.cmds[SETTINGS_VCD].cmdstr = 
	    &dsbcfg_getval(cfg, CFG_PLAY_VCD).string;

	settingsmenu.cmds[SETTINGS_SVCD].autoplay =
	    (bool *)&dsbcfg_getval(cfg, CFG_SVCD_AUTO).boolean;
	settingsmenu.cmds[SETTINGS_SVCD].cmdstr = 
	    &dsbcfg_getval(cfg, CFG_PLAY_SVCD).string;

	settingsmenu.cmds[SETTINGS_CDDA].autoplay =
	    (bool *)&dsbcfg_getval(cfg, CFG_CDDA_AUTO).boolean;
	settingsmenu.cmds[SETTINGS_CDDA].cmdstr = 
	    &dsbcfg_getval(cfg, CFG_PLAY_CDDA).string;
	settingsmenu.ignore_list =
	    &dsbcfg_getval(cfg, CFG_HIDE).strings;
	path = PATH_DSBMD_SOCKET;
	for (i = 0; i < 10 && (sock = uconnect(path)) == NULL; i++) {
		if (errno == EINTR || errno == ECONNREFUSED)
			(void)sleep(1);
		else
			xerr(NULL, EXIT_FAILURE, "Couldn't connect to DSBMD");
	}
	if (i == 10)
		xerr(NULL, EXIT_FAILURE, "Couldn't connect to DSBMD");

	/* Get the drive list from dsbmd. */
	for (ndrives = 0, p = readln(true); p[0] != '='; p = readln(true)) {
		if (parse_dsbmdevent(p) == -1)
			continue;
		if (dsbmdevent.type == EVENT_ADD_DEVICE) {
			add_drive(&dsbmdevent.drvinfo);
		} else if (dsbmdevent.type == EVENT_ERROR_MSG) {
			if (dsbmdevent.code != ERR_PERMISSION_DENIED)
				continue;
			xerrx(NULL, EXIT_FAILURE,
			    _("You are not allowed to connect to DSBMD"));
		} else if (dsbmdevent.type == EVENT_SHUTDOWN) {
			xerrx(NULL, EXIT_FAILURE, _("DSBMD just shut down."));
		}
	}
	ioc   = g_io_channel_unix_new(fileno(sock));
	iotag = g_io_add_watch(ioc, G_IO_IN, readevent, NULL);

	(void)signal(SIGCHLD, catch_child);
	(void)signal(SIGTERM, cleanup);
	(void)signal(SIGINT, cleanup);
	(void)signal(SIGHUP, cleanup);
	(void)signal(SIGQUIT, cleanup);

	/* Init proc table */
	for (i = 0; i < NPROCS; i++)
		proctbl[i].pid = -1;

	create_mainwin();

	for (;;) {
		gtk_main();
		/* Block SIGCHLD */
		sigemptyset(&sigmask); sigaddset(&sigmask, SIGCHLD);
		sigprocmask(SIG_BLOCK, &sigmask, NULL);

		for (i = 0; i < NPROCS; i++) {
			if (proctbl[i].pid == -1)
				continue;
			if (proctbl[i].error == EXIT_ENOENT) {
				/* We couldn't execute the program. */
				errno = ENOENT;
				xwarn(mainwin.win,
				    _("Couldn't execute command \"%s\""),
				    proctbl[i].cmdstr);
				free(proctbl[i].cmdstr);
				proctbl[i].pid = -1;
			} else if (proctbl[i].error == EXIT_EXEC_ERR) {
				errno = proctbl[i].saved_errno;
				xwarn(mainwin.win,
				    _("Failed to execute shell \"%s\""),
				    _PATH_BSHELL);
				free(proctbl[i].cmdstr);
				proctbl[i].pid = -1;
			}
		}
		/* Unblock SIGCHLD. */
		sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
	}
}

static void
usage()
{
	(void)printf("Usage: %s [-ih] [<disk image> ...]\n" \
		     "   -i: Start %s as tray icon\n", PROGRAM, PROGRAM);
	exit(EXIT_FAILURE);
}

static void
create_mainwin()
{
	GdkPixbuf *icon;
	GtkWidget *menu, *root_menu, *menu_bar, *item, *sw, *image, *vbox;

	if ((icon = load_icon(32, "drive-harddisk-usb",
	    "drive-removable-media", "drive-harddisk", NULL)) == NULL) {
		icon = load_icon(32, "image-missing", NULL);
	}
	mainwin.win = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
	gtk_window_set_default_size(mainwin.win, *mainwin.width,
	    *mainwin.height);
	gtk_window_set_title(mainwin.win, TITLE);
	gtk_window_set_resizable(mainwin.win, TRUE);
	gtk_window_set_icon(mainwin.win, icon);

	load_pixbufs();
	create_icon_list();

	/* Create the menu for the menu bar and the tray icon. */
	menu  = gtk_menu_new();
	image = gtk_image_new_from_icon_name("preferences-system", GTK_ICON_SIZE_MENU);
	item  = gtk_image_menu_item_new_with_mnemonic(_("_Preferences"));
	gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(item), image);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	g_signal_connect(G_OBJECT(item), "activate",
	    G_CALLBACK(settings_menu), NULL);

	image = gtk_image_new_from_icon_name("application-exit", GTK_ICON_SIZE_MENU);
	item  = gtk_image_menu_item_new_with_mnemonic(_("_Quit"));
	gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(item), image);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	g_signal_connect(G_OBJECT(item), "activate",
	    G_CALLBACK(cleanup), NULL);
	root_menu = gtk_menu_item_new_with_mnemonic(_("_File"));
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(root_menu), menu);

	menu_bar = gtk_menu_bar_new();
	gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), root_menu);
	gtk_widget_show(menu_bar);

	mainwin.store = gtk_list_store_new(NUM_COLS, G_TYPE_STRING,
		    GDK_TYPE_PIXBUF, G_TYPE_POINTER, G_TYPE_STRING);

	mainwin.icon_view = 
	    gtk_icon_view_new_with_model(GTK_TREE_MODEL(mainwin.store));
	gtk_icon_view_set_text_column(GTK_ICON_VIEW(mainwin.icon_view),
	    COL_NAME);
	gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(mainwin.icon_view),
	    COL_PIXBUF);
	mainwin.store = create_icontbl(mainwin.store);

	sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
	    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(sw), mainwin.icon_view);

	mainwin.statusbar = gtk_statusbar_new();
#if GTK_MAJOR_VERSION < 3
	gtk_statusbar_set_has_resize_grip(GTK_STATUSBAR(mainwin.statusbar),
	    FALSE);
#endif

#if GTK_MAJOR_VERSION < 3
	vbox = gtk_vbox_new(FALSE, 0);
#else
	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
#endif
	gtk_box_pack_start(GTK_BOX(vbox), menu_bar, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), mainwin.statusbar, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(mainwin.win), vbox);

	mainwin.tray_icon = gtk_status_icon_new_from_pixbuf(icon);
	g_signal_connect(G_OBJECT(mainwin.icon_view),
                    "button-press-event", G_CALLBACK(icon_clicked), NULL);
 	g_signal_connect(G_OBJECT(mainwin.tray_icon), "activate",
	    G_CALLBACK(tray_click), mainwin.win);
	g_signal_connect(G_OBJECT(mainwin.tray_icon), "popup-menu",
	    G_CALLBACK(popup_tray_ctxmenu), G_OBJECT(menu));
	gtk_status_icon_set_tooltip_text(mainwin.tray_icon, "DSBMC");
	gtk_status_icon_set_visible(mainwin.tray_icon, TRUE);

	g_signal_connect(mainwin.win, "delete-event",
	    G_CALLBACK(hide_win), NULL);
	g_signal_connect(G_OBJECT(mainwin.win), "window-state-event",
	    G_CALLBACK(window_state_event), mainwin.tray_icon);
	g_signal_connect(G_OBJECT(mainwin.win), "configure-event",
	    G_CALLBACK(window_state_event), mainwin.win);
	if (*mainwin.posx >= 0 && *mainwin.posy >= 0)
		gtk_window_move(GTK_WINDOW(mainwin.win),
		    *mainwin.posx, *mainwin.posy);
	if (mainwin.win_state != GDK_WINDOW_STATE_WITHDRAWN)  
		gtk_widget_show_all(GTK_WIDGET(mainwin.win));
}

static void
hide_win(GtkWidget *win)
{       
	gtk_window_get_position(GTK_WINDOW(win), mainwin.posx, mainwin.posy);
	if (*mainwin.posx < 0 || *mainwin.posy < 0  ||
	    gdk_screen_width() - *mainwin.posx < 0 ||
	    gdk_screen_height() - *mainwin.posy < 0)
		*mainwin.posx = *mainwin.posy = 0;
	gtk_window_get_size(GTK_WINDOW(win), mainwin.width, mainwin.height);
	gtk_widget_hide(win);
}

static gboolean
window_state_event(GtkWidget *wdg, GdkEvent *event, gpointer unused)
{       
	switch ((int)event->type) {
	case GDK_CONFIGURE:
		*mainwin.posx   = ((GdkEventConfigure *)event)->x;
		*mainwin.posy   = ((GdkEventConfigure *)event)->y;
		*mainwin.width  = ((GdkEventConfigure *)event)->width;
		*mainwin.height = ((GdkEventConfigure *)event)->height;
		dsbcfg_write(PROGRAM, PATH_CONFIG, cfg);
		break;
	case GDK_WINDOW_STATE:
		mainwin.win_state =
		    ((GdkEventWindowState *)event)->new_window_state;
		dsbcfg_write(PROGRAM, PATH_CONFIG, cfg);
		break;
	}
	return (FALSE);
}

static void
tray_click(GtkStatusIcon *status_icon, gpointer win)
{
	if ((mainwin.win_state & GDK_WINDOW_STATE_ICONIFIED) ||
	    (mainwin.win_state & GDK_WINDOW_STATE_WITHDRAWN) ||
	    (mainwin.win_state & GDK_WINDOW_STATE_BELOW)) {
		if ((mainwin.win_state & GDK_WINDOW_STATE_ICONIFIED))
			gtk_window_deiconify(win);
		if (*mainwin.posx >= 0 && *mainwin.posy >= 0) {
			gtk_window_move(GTK_WINDOW(win),
			    *mainwin.posx, *mainwin.posy);
		}
		gtk_widget_show_all(GTK_WIDGET(win));
	} else {
		gtk_window_get_position(GTK_WINDOW(win),
		    mainwin.posx, mainwin.posy);
		if (*mainwin.posx < 0   || *mainwin.posy < 0 ||
		    gdk_screen_width()  - *mainwin.posx < 0 ||
		    gdk_screen_height() - *mainwin.posy < 0)
			*mainwin.posx = *mainwin.posy = 0;
		gtk_widget_hide(GTK_WIDGET(win));
	}
}

static void
popup_tray_ctxmenu(GtkStatusIcon *icon, guint bt, guint tm, gpointer menu)
{       
	gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, bt, tm);
}

static void
settings_menu()
{
	int	     i, j;
	char	     *s, *q, *qs, **v;
	bool	     error;
	size_t	     len;
	drive_t	     *dp;
	const char   *p;
	GtkWidget    *win, *abt, *cbt, *cb, *label, *table, *image;
	GtkWidget    *entry[SETTINGS_NCMDS + 1];

	win = gtk_dialog_new();
	gtk_window_set_title(GTK_WINDOW(win), _("Preferences"));
	gtk_window_set_icon_name(GTK_WINDOW(win), "preferences-system");
	gtk_container_set_border_width(GTK_CONTAINER(win), 10);

	table = gtk_table_new(SETTINGS_NCMDS + 1, 5, FALSE);
	gtk_table_set_col_spacing(GTK_TABLE(table), 2, 10);
	gtk_table_set_col_spacing(GTK_TABLE(table), 0, 10);

	for (i = 0; i < SETTINGS_NCMDS; i++) {
		for (j = 0; j < PIXBUFTBLSZ; j++) {
			if (strcmp(settingsmenu.cmds[i].iconid,
			    pixbuftbl[j].id) == 0) {
				image = gtk_image_new_from_pixbuf(
				    pixbuftbl[j].icon);
				break;
			}
		}
		label = new_label(ALIGN_LEFT, ALIGN_CENTER,
		    _(settingsmenu.cmds[i].action));
		entry[i] = gtk_entry_new();
		gtk_entry_set_text(GTK_ENTRY(entry[i]),
		    *settingsmenu.cmds[i].cmdstr);
		gtk_entry_set_width_chars(GTK_ENTRY(entry[i]), 35);
		if (settingsmenu.cmds[i].autoplay != NULL) {
			cb = gtk_check_button_new_with_label("Autoplay");
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb),
			    *settingsmenu.cmds[i].autoplay);
			gtk_table_attach(GTK_TABLE(table), cb, 3, 4, i, i + 1,
			    GTK_EXPAND |GTK_FILL, 0, 0, 0);
			g_signal_connect(cb, "toggled", G_CALLBACK(cb_cb),
			    settingsmenu.cmds[i].autoplay);
		}
		gtk_table_attach(GTK_TABLE(table), image, 0, 1, i, i + 1,
		    GTK_FILL, 0, 0, 0);
		gtk_table_attach(GTK_TABLE(table), label, 1, 2, i, i + 1,
		    GTK_FILL, 0, 0, 0);
	 	gtk_table_attach(GTK_TABLE(table), entry[i], 2, 3, i, i + 1,
		    GTK_EXPAND |GTK_FILL, 0, 0, 0);
		label = new_pango_label(ALIGN_LEFT, ALIGN_CENTER,
			    _(SETTINGS_MENU_INFO_MSG));
		gtk_widget_set_tooltip_text(GTK_WIDGET(entry[i]),
		    gtk_label_get_text(GTK_LABEL(label)));
	}
	for (j = 0; j < PIXBUFTBLSZ; j++) {
		if (strcmp("hide", pixbuftbl[j].id) == 0) {
			image = gtk_image_new_from_pixbuf(pixbuftbl[j].icon);
			break;
		}
	}
	label = new_label(ALIGN_LEFT, ALIGN_CENTER,
	    _("Ignore mount points/devices:"));
	entry[i] = gtk_entry_new();

	for (len = 0, v = *settingsmenu.ignore_list;
	    v != NULL && *v != NULL; v++)
		len += strlen(*v) + 4;
	if (len > 0) {
		if ((s = malloc(len * 2)) == NULL)
			xerr(mainwin.win, EXIT_FAILURE, "malloc()");
		(void)memset(s, 0, len);
		for (v = *settingsmenu.ignore_list;
		    v != NULL && *v != NULL; v++) {
			qs = g_strdup_printf("%s\"%s\"",
			    v != *settingsmenu.ignore_list ? ", " : "", *v);
			if (qs == NULL) {
				xerr(mainwin.win, EXIT_FAILURE,
				    "g_strdup_printf()");
			}
			(void)strncat(s, qs, len);
			len -= strlen(qs);
			free(qs);
		}
		gtk_entry_set_text(GTK_ENTRY(entry[i]), s);
		free(s);
	}
	gtk_widget_set_tooltip_text(GTK_WIDGET(entry[i]),
	    _("Comma separated list of mount points/devices to ignore\n" \
	      "E.g.: /var/run/user/1001/gvfs, /dev/ada0p2, ..."));
	gtk_entry_set_width_chars(GTK_ENTRY(entry[i]), 35);
	gtk_table_attach(GTK_TABLE(table), image, 0, 1, i, i + 1,
		    GTK_FILL, 0, 0, 0);
 	gtk_table_attach(GTK_TABLE(table), label, 1, 2, i, i + 1,
	    GTK_FILL, 0, 0, 0);	
	gtk_table_attach(GTK_TABLE(table), entry[i], 2, 3, i, i + 1,
	    GTK_EXPAND | GTK_FILL, 0, 0, 0);

	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(win))), table,
	    TRUE, TRUE, 0);
	abt = gtk_button_new_with_mnemonic(_("_Ok"));
	cbt = gtk_button_new_with_mnemonic(_("_Cancel"));
	gtk_dialog_add_action_widget(GTK_DIALOG(win), abt,
	    GTK_RESPONSE_ACCEPT);
	gtk_dialog_add_action_widget(GTK_DIALOG(win), cbt,
	    GTK_RESPONSE_REJECT);
	gtk_widget_show_all(win);

	/* Unselect all entries. */
	for (i = 0; i < SETTINGS_NCMDS + 1; i++)
		gtk_editable_select_region(GTK_EDITABLE(entry[i]), 0, 0);
	for (;;) {
		if (gtk_dialog_run(GTK_DIALOG(win)) != GTK_RESPONSE_ACCEPT)
			break;
		for (i = 0; i < SETTINGS_NCMDS; i++) {
			p = gtk_entry_get_text(GTK_ENTRY(entry[i]));
			q = *settingsmenu.cmds[i].cmdstr;
			if (strcmp(p, q) == 0)
				continue;
			free(q);
			q = *settingsmenu.cmds[i].cmdstr = g_strdup(p);
			if (q == NULL) {
				xerr(GTK_WINDOW(win), EXIT_FAILURE,
				    "g_strdup()");
			}
		}
		p = gtk_entry_get_text(GTK_ENTRY(entry[SETTINGS_NCMDS]));
		v = dsbcfg_list_to_strings(p, &error);
		if (v == NULL && error) {
			xwarnx(GTK_WINDOW(win), "%s", dsbcfg_strerror());
			continue;
		}
		dsbcfg_setval(cfg, CFG_HIDE, DSBCFG_VAL(v));
		create_icon_list();
		dsbcfg_write(PROGRAM, PATH_CONFIG, cfg);
		for (v = dsbcfg_getval(cfg, CFG_HIDE).strings;
		    v != NULL && *v != NULL; v++) {
			dp = lookupdrv_from_mnt(*v);
			if (dp != NULL) {
				del_icon(dp->dev);
				(void)create_icontbl(mainwin.store);
			} else if ((dp = lookupdrv(*v)) != NULL) {
				del_icon(dp->dev);
                                (void)create_icontbl(mainwin.store);
			}
		}
		(void)create_icontbl(mainwin.store);
		if ((mainwin.win_state & GDK_WINDOW_STATE_ICONIFIED) ||
		    (mainwin.win_state & GDK_WINDOW_STATE_WITHDRAWN)) {
			gtk_widget_show_all(GTK_WIDGET(mainwin.win));
			gtk_widget_hide(GTK_WIDGET(mainwin.win));
		} else
			gtk_widget_show_all(GTK_WIDGET(mainwin.win));
		gtk_widget_destroy(win);
		return;
	}
	gtk_widget_destroy(win);
}

static void
cleanup(int unused)
{

	dsbcfg_write(PROGRAM, PATH_CONFIG, cfg);
	gtk_main_quit();
	exit(0);
}

static ctxmenu_t *
create_ctxmenu(icon_t *icon)
{
	int	  i, j;
	ctxmenu_t *ctxmenu;
	GtkWidget *item, *image;

	if ((ctxmenu = malloc(sizeof(ctxmenu_t))) == NULL)
		return (NULL);
	ctxmenu->menu = gtk_menu_new();

	for (i = 0; i < NMENU_ITEMS; i++)
		ctxmenu->menuitems[i] = NULL;
	for (i = 0; i < NMENUCMDS; i++) {
     		for (j = 0; j < NCMDS; j++) {
			if (menucmds[i].cmd == cmdtbl[j].cmd)
				break;
		}
		if (j == NCMDS || !(icon->drvp->cmds & cmdtbl[j].cmd))
			continue;
		item  = gtk_image_menu_item_new_with_mnemonic(
		    _(menucmds[i].name));
		image = gtk_image_new_from_pixbuf(cmdtbl[j].pix);
		gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(item),
		   image);
		gtk_menu_shell_append(GTK_MENU_SHELL(ctxmenu->menu), item);
		g_signal_connect(G_OBJECT(item), "activate",
		    G_CALLBACK(menucmds[i].cb), icon);
		if ((cmdtbl[j].cmd & DRVCMD_MOUNT))
			ctxmenu->menuitems[MENU_ITEM_MOUNT] = item;
		else if ((cmdtbl[j].cmd & DRVCMD_UNMOUNT))
			ctxmenu->menuitems[MENU_ITEM_UNMOUNT] = item;
		gtk_widget_show(item);
	}
	if (i == 0) {
		gtk_widget_destroy(ctxmenu->menu);
		free(ctxmenu);
		return (NULL);
	}
	return (ctxmenu);
}

/*
 * Adds a new device icon to the icon list and returns a pointer to it.
 */
static icon_t *
add_icon(drive_t *drvp)
{
	int i, j, k;

	for (i = 0; i < nicons; i++) {
		if (icons[i]->drvp == drvp)
			return (NULL);
	}
	for (i = 0; i < NDSKTYPES; i++) {
		if (disktypetbl[i].type == drvp->type)
			break;
	}
	if (i == NDSKTYPES)
		return (NULL);
	icons = realloc(icons, (nicons + 1) * sizeof(icon_t *));
	if (icons == NULL)
		return (NULL);
	for (j = 0; j < nicons; j++) {
		if (strcasecmp(drvp->volid, icons[j]->drvp->volid) < 0)
			break;
	}
	for (k = nicons; k > j; k--)
		icons[k] = icons[k - 1];
	if ((icons[j] = malloc(sizeof(icon_t))) == NULL)
		return (NULL);
	icons[j]->drvp	      = drvp;
	icons[j]->ctxmenu     = create_ctxmenu(icons[j]);
	icons[j]->pix_normal  = disktypetbl[i].pix_normal;
	icons[j]->pix_mounted = disktypetbl[i].pix_mounted;

	if (drvp->mounted)
		icons[j]->pixbuf = disktypetbl[i].pix_mounted;
	else
		icons[j]->pixbuf = disktypetbl[i].pix_normal;
	nicons++;
	return (icons[j]);
}

static void
del_icon(const char *devname)
{
	int i;

	for (i = 0; i < nicons; i++) {
		if (strcmp(icons[i]->drvp->dev, devname) == 0)
			break;
	}
	if (i == nicons)
		return;
	gtk_widget_destroy(icons[i]->ctxmenu->menu);
	free(icons[i]->ctxmenu);
	free(icons[i]);
	for (; i < nicons - 1; i++)
		icons[i] = icons[i + 1];
	nicons--;
}

static void
create_icon_list()
{
	int i;

	for (i = 0; i < ndrives; i++)
		(void)add_icon(drives[i]);
}

static gboolean
icon_clicked(GtkWidget *widget, GdkEvent *event, gpointer data)
{       
	icon_t	       *icon;
	GtkTreeIter    iter;
	GtkTreePath    *path;
	GdkEventButton *bevent;

	bevent = (GdkEventButton *)event;
	icon = (icon_t *)data;

	if (event->type == GDK_2BUTTON_PRESS) {
		/* Double click */
		path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(
		    mainwin.icon_view), bevent->x, bevent->y);
		if (path == NULL) {
			gtk_statusbar_push(GTK_STATUSBAR(mainwin.statusbar),
			    0, "");
			return (FALSE);
		}
		gtk_tree_model_get_iter(GTK_TREE_MODEL(mainwin.store),
		    &iter, path);
		gtk_tree_model_get(GTK_TREE_MODEL(mainwin.store), &iter,
		    COL_ICON, &icon, -1);
		if ((icon->drvp->cmds  & DRVCMD_PLAY) &&
		    !(icon->drvp->cmds & DRVCMD_MOUNT))
			cb_play(NULL, icon);
		else if ((icon->drvp->cmds & DRVCMD_MOUNT))
			cb_open(NULL, icon);
	} else if (event->type == GDK_BUTTON_PRESS && bevent->button == 3) {
		/* Right mouse click on icon. */
		path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(
		    mainwin.icon_view), bevent->x, bevent->y);
		if (path == NULL) {
			gtk_statusbar_push(GTK_STATUSBAR(mainwin.statusbar),
			    0, "");
			return (FALSE);
		}
		gtk_tree_model_get_iter(GTK_TREE_MODEL(mainwin.store),
		    &iter, path);
		gtk_tree_model_get(GTK_TREE_MODEL(mainwin.store), &iter,
		    COL_ICON, &icon, -1);
		gtk_icon_view_unselect_all(GTK_ICON_VIEW(mainwin.icon_view));
		gtk_icon_view_select_path(GTK_ICON_VIEW(mainwin.icon_view),
		    path);
		/* Get the disk size. */
		cb_size(NULL, icon);
		if ((icon->drvp->cmds & DRVCMD_MOUNT) &&
		    icon->drvp->mounted) {
			/*
			 * Drive  is mounted - Set "Mount"-label in the context
			 * menu insensitive, and set "Unmount"-label sensitive.
			 */
			gtk_widget_set_sensitive(
			    icon->ctxmenu->menuitems[MENU_ITEM_MOUNT], FALSE);
			gtk_widget_set_sensitive(
			    icon->ctxmenu->menuitems[MENU_ITEM_UNMOUNT], TRUE);
		} else if ((icon->drvp->cmds & DRVCMD_MOUNT)) {
			/*
			 * Drive is not mounted - Set "Mount"-label in the con-
			 * text  menu sensitive, and set "Unmount"-label insen-
			 * sitive.
			 */
			gtk_widget_set_sensitive(
			    icon->ctxmenu->menuitems[MENU_ITEM_UNMOUNT], FALSE);
			gtk_widget_set_sensitive(
			    icon->ctxmenu->menuitems[MENU_ITEM_MOUNT], TRUE);
		}
#if GTK_MAJOR_VERSION < 3
		gtk_menu_popup(GTK_MENU(icon->ctxmenu->menu), NULL, NULL,
		    NULL, NULL, bevent->button, bevent->time);
#else
		gtk_menu_popup_at_pointer(GTK_MENU(icon->ctxmenu->menu), event);
#endif
	} else if (event->type == GDK_BUTTON_PRESS && bevent->button == 1) {
		/* Left mouse button pressed. */
		path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(
		    mainwin.icon_view), bevent->x, bevent->y);
		if (path == NULL) {
			gtk_statusbar_push(GTK_STATUSBAR(mainwin.statusbar),
			    0, "");
			return (FALSE);
		}
		gtk_tree_model_get_iter(GTK_TREE_MODEL(mainwin.store),
		    &iter, path);
		gtk_tree_model_get(GTK_TREE_MODEL(mainwin.store), &iter,
		    COL_ICON, &icon, -1);
		cb_size(NULL, icon);
	}
	return (FALSE);
}

/*
 * Creates (store == NULL), shrinks or extends a GtkTable, and adds
 * the device icons to it.
 */
static GtkListStore *
create_icontbl(GtkListStore *store)
{
	int	    i;
	GtkTreeIter iter;

	if (store != NULL) {
		/* Create a new table. */
		gtk_list_store_clear(GTK_LIST_STORE(store));
	} else {
		store = gtk_list_store_new(NUM_COLS, G_TYPE_STRING,
		    GDK_TYPE_PIXBUF, G_TYPE_POINTER, G_TYPE_STRING);
	}
	for (i = 0; i < nicons; i++) {
		gtk_list_store_append(GTK_LIST_STORE(store), &iter);
		gtk_list_store_set(GTK_LIST_STORE(store), &iter,
		    COL_NAME, icons[i]->drvp->volid,
		    COL_PIXBUF, icons[i]->pixbuf,
		    COL_ICON, icons[i], -1);
		icons[i]->iter = iter;
	}
	return (store);
}

static void
update_icons()
{
	int i;

	for (i = 0; i < nicons; i++)
		set_mounted(icons[i], icons[i]->drvp->mounted);
}

static void
load_pixbufs()
{
	int	      i, j;
	GdkPixbuf    *icon;
	GtkIconTheme *icon_theme;

	icon_theme = gtk_icon_theme_get_default();
	for (i = 0; i < PIXBUFTBLSZ; i++) {
		for (j = 0; pixbuftbl[i].name[j] != NULL; j++) {
			icon = gtk_icon_theme_load_icon(icon_theme,
			    pixbuftbl[i].name[j], pixbuftbl[i].iconsize,
			    ICON_LOOKUP_FLAGS, NULL);
			if (icon != NULL)
				break;
		}
		if (icon == NULL) {
			icon = gtk_icon_theme_load_icon(icon_theme,
			    "missing-image", pixbuftbl[i].iconsize,
			    ICON_LOOKUP_FLAGS, NULL);
		}
		pixbuftbl[i].icon = icon;
	}
	for (i = 0; i < NDSKTYPES; i++) {
		disktypetbl[i].pix_normal  = lookup_pixbuf(disktypetbl[i].name);
		disktypetbl[i].pix_mounted = lookup_pixbuf("mounted");
	}
	for (i = 0; i < NCMDS; i++)
		cmdtbl[i].pix = lookup_pixbuf(cmdtbl[i].name);
}

static GdkPixbuf *
lookup_pixbuf(const char *id)
{
	int i;

	for (i = 0; i < PIXBUFTBLSZ; i++) {
		if (strcmp(pixbuftbl[i].id, id) == 0)
			return (pixbuftbl[i].icon);
	}
	return (NULL);
}

static void
busywin(const char *msg, bool show)
{
	static GtkWidget *spinner, *label, *hbox, *win = NULL;

	if (!show) {
		if (win != NULL)
			gtk_widget_destroy(win);
		win = NULL;
		return;
	}
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
	gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(mainwin.win));
	gtk_window_set_modal(GTK_WINDOW(win), TRUE);
	gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER_ON_PARENT);
	gtk_container_set_border_width(GTK_CONTAINER(win), 10);

	label = new_label(ALIGN_CENTER, ALIGN_CENTER, msg);

	spinner = gtk_spinner_new();
	gtk_spinner_start(GTK_SPINNER(spinner));

#if GTK_MAJOR_VERSION < 3
	hbox = gtk_hbox_new(FALSE, 5);
#else
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
#endif
	gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), spinner, TRUE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(win), hbox);
	gtk_widget_show_all(win);
}

static void
set_mounted(icon_t *icon, bool mounted)
{
	if (mounted)
		icon->pixbuf = icon->pix_mounted;
	else
		icon->pixbuf = icon->pix_normal;
	gtk_list_store_set(GTK_LIST_STORE(mainwin.store), &icon->iter,
	    COL_PIXBUF, icon->pixbuf, -1);
}

static const char *
errmsg(int error)
{
	int i;

	for (i = 0; i < NERRCODES; i++) {
		if (errorcodes[i].error == error)
			return (errorcodes[i].msg);
	}
	if (error < (1 << 8))
		return (strerror(error));
	return (NULL);
}

static char *
readln(bool block)
{
	char	    *p;
	fd_set	    rset;
	static char buf[_POSIX2_LINE_MAX];

	if (fgets(buf, sizeof(buf), sock) == NULL) {
		if (feof(sock)) {
			xerrx(mainwin.win, EXIT_FAILURE,
			    _("Lost connection to DSBMD"));
		} else if (!block)
			return (NULL);
	} else
		return (buf);
	FD_ZERO(&rset); FD_SET(fileno(sock), &rset);
	/* Block until data is available. */
	while (select(fileno(sock) + 1, &rset, NULL, NULL, NULL) == -1) {
		if (errno != EINTR)
			return (NULL);
	}
	if ((p = fgets(buf, sizeof(buf), sock)) == NULL)
		xerrx(mainwin.win, EXIT_FAILURE, _("Lost connection to DSBMD"));
	return (p);
}

static drive_t *
lookupdrv(const char *devname)
{
	int i;

	if (devname == NULL)
		return (NULL);
	for (i = 0; i < ndrives; i++) {
		if (strcmp(drives[i]->dev, devname) == 0)
			return (drives[i]);
	}
	return (NULL);
}

static drive_t *
lookupdrv_from_mnt(const char *mnt)
{
	int i;

	if (mnt == NULL)
		return (NULL);
	for (i = 0; i < ndrives; i++) {
		if (drives[i]->mntpt == NULL)
			continue;
		if (strcmp(drives[i]->mntpt, mnt) == 0)
			return (drives[i]);
	}
	return (NULL);

}

static gboolean
readevent(GIOChannel *ioc, GIOCondition cond, gpointer data)
{
	char *p;

	while ((p = readln(false)) != NULL) {
		switch (process_event(p)) {
		case EVENT_SUCCESS_MSG:
		case EVENT_ERROR_MSG:
			call_reply_function();
		}
	}
	return (TRUE);
}

static void
catch_child(int signo)
{
	int   i, status;
	pid_t pid;

	if ((pid = waitpid(-1, &status, WNOHANG)) != -1 && WIFEXITED(status)) {
		for (i = 0; i < NPROCS; i++) {
			if (pid == proctbl[i].pid) {
				proctbl[i].error = WEXITSTATUS(status);
				if (proctbl[i].error == EXIT_ENOENT ||
				    proctbl[i].error == EXIT_EXEC_ERR)
					gtk_main_quit();
				else {
					/* Child exited successfully. */
					free(proctbl[i].cmdstr);
					proctbl[i].pid = -1;
				}
				return;
			}
		}
	}
}

/*
 * Parses the command string and executes the command with parameters taken
 * from the given drive.
 */
static void
exec_cmd(const char *cmdstr, drive_t *drvp)
{
	int	 i, n, len;
	char	 *cmdbuf, *p;
	pid_t	 pid;
	sigset_t sigmask, savedmask;

	/* try to find a free slot. */
	for (i = 0; i < NPROCS; i++) {
		if (proctbl[i].pid == -1)
			break;
	}
	if (i == NPROCS) {
		xwarnx(mainwin.win, _("Maximal number of processes reached"));
		return;
	}
	if ((cmdbuf = malloc(_POSIX2_LINE_MAX)) == NULL)
		xerr(mainwin.win, EXIT_FAILURE, "malloc()");
	for (len = _POSIX2_LINE_MAX, p = cmdbuf; *cmdstr != '\0' && len > 0;) {
		if (*cmdstr == '%') {
			switch (*++cmdstr) {
			case 'd':
				n = strlen(drvp->dev);
				if (len - n - 1 < 0) {
					xwarn(mainwin.win,
					    _("Command string too long"));
					goto cleanup;
				}
				(void)strncpy(p, drvp->dev, n);
				len -= n; p += n;
				break;
			case 'm':
				if (drvp->mntpt != NULL)
					n = strlen(drvp->mntpt);
				else
					n = 0;
				if (len - n - 1 < 0) {
					xwarn(mainwin.win,
					    _("Command string too long"));
					goto cleanup;
				}
				if (n > 0) {
					(void)snprintf(p, n + 1, "%s",
					    drvp->mntpt);
				}
				len -= n; p += n;
				break;
			default:
				xwarnx(mainwin.win,
				    _("Unknown field code '%%%c'"), *cmdstr);
				goto cleanup;
			}
		} else {
			*p++ = *cmdstr;
			len--;
		}
		cmdstr++;
	}
	*p = '\0';

	if (*cmdstr != '\0') {
		xwarn(mainwin.win, _("Command string too long"));
		goto cleanup;
	}
	/* Block SIGCHLD */
	(void)sigemptyset(&sigmask); (void)sigaddset(&sigmask, SIGCHLD);
	(void)sigprocmask(SIG_BLOCK, &sigmask, &savedmask);

	switch ((pid = vfork())) {
	case -1:
		xerr(mainwin.win, EXIT_FAILURE, "vfork()");
		/* NOTREACHED */
	case  0:
		/* Restore old signal mask */
		(void)sigprocmask(SIG_SETMASK, &savedmask, NULL);
		execl(_PATH_BSHELL, _PATH_BSHELL, "-c", cmdbuf, NULL);
		proctbl[i].saved_errno = errno;
		_exit(255);
		/* NOTREACHED */
	default:
		proctbl[i].pid = pid;
		if ((proctbl[i].cmdstr = strdup(cmdbuf)) == NULL)
			xerr(mainwin.win, EXIT_FAILURE, "strdup()");
		/* Restore old signal mask */
		(void)sigprocmask(SIG_SETMASK, &savedmask, NULL);
		break;
	}
cleanup:
	free(cmdbuf);
}

/*
 * Parses  a  string  read from the dsbmd socket. Depending on the event type,
 * process_event() adds or deletes drives from the list, or updates a drive's
 * status.
 */
static int
process_event(char *buf)
{
	char	*cmd = NULL;
	drive_t *drvp;

	if (parse_dsbmdevent(buf) != 0)
		return (-1);
	if (dsbmdevent.type == EVENT_ADD_DEVICE) {
		if ((drvp = add_drive(&dsbmdevent.drvinfo)) == NULL)
			return (dsbmdevent.type);
		(void)add_icon(drvp);
		(void)create_icontbl(mainwin.store);
		gtk_window_deiconify(GTK_WINDOW(mainwin.win));
		gtk_widget_show_all(GTK_WIDGET(mainwin.win));
		switch (drvp->type) {
		case DSKTYPE_AUDIOCD:
			if (dsbcfg_getval(cfg, CFG_CDDA_AUTO).boolean)
				cmd = dsbcfg_getval(cfg, CFG_PLAY_CDDA).string;
			break;
		case DSKTYPE_DVD:
			if (dsbcfg_getval(cfg, CFG_DVD_AUTO).boolean)
				cmd = dsbcfg_getval(cfg, CFG_PLAY_DVD).string;
			break;
		case DSKTYPE_VCD:
			if (dsbcfg_getval(cfg, CFG_VCD_AUTO).boolean)
				cmd = dsbcfg_getval(cfg, CFG_PLAY_VCD).string;
			break;
		case DSKTYPE_SVCD:
			if (dsbcfg_getval(cfg, CFG_SVCD_AUTO).boolean)
				cmd = dsbcfg_getval(cfg, CFG_PLAY_SVCD).string;
			break;
		default:
			cmd = NULL;
		}
		if (cmd != NULL && *cmd != '\0')
			exec_cmd(cmd, drvp);
	} else if (dsbmdevent.type == EVENT_DEL_DEVICE) {
		del_icon(dsbmdevent.drvinfo.dev);
		del_drive(dsbmdevent.drvinfo.dev);
		(void)create_icontbl(mainwin.store);
		if (nicons == 0)
			hide_win(GTK_WIDGET(mainwin.win));
	} else if (dsbmdevent.type == EVENT_MOUNT) {
		if ((drvp = lookupdrv(dsbmdevent.drvinfo.dev)) == NULL)
			return (dsbmdevent.type);
		drvp->mounted = true;
		free(drvp->mntpt);
		drvp->mntpt = strdup(dsbmdevent.drvinfo.mntpt);
		update_icons();
	} else if (dsbmdevent.type == EVENT_UNMOUNT) {
		if ((drvp = lookupdrv(dsbmdevent.drvinfo.dev)) == NULL)
				return (dsbmdevent.type);
			drvp->mounted = false;
			free(drvp->mntpt);
			drvp->mntpt = NULL;
			update_icons();
	} else if (dsbmdevent.type == EVENT_SPEED) {
		if ((drvp = lookupdrv(dsbmdevent.drvinfo.dev)) == NULL)
			return (dsbmdevent.type);
		drvp->speed = dsbmdevent.drvinfo.speed;
	} else if (dsbmdevent.type == EVENT_SHUTDOWN)
		xerrx(NULL, EXIT_FAILURE, _("DSBMD just shut down."));
	return (dsbmdevent.type);
}

/*
 * Adds a mount point to the ~/.gtk-bookmarks file.
 */
static void
add_bookmark(const char *bookmark)
{
	bool	      found;
	char	      *path, *ln;
	FILE	      *fp;
	struct passwd *pw;

	if ((pw = getpwuid(getuid())) == NULL) {
		xerrx(mainwin.win, EXIT_FAILURE,
		    _("Couldn't find you in the password file."));
	}
	endpwent();
	if ((ln = malloc(_POSIX2_LINE_MAX)) == NULL)
		xerr(mainwin.win, EXIT_FAILURE, "malloc()");
	path = g_strdup_printf("%s/%s", pw->pw_dir, PATH_BOOKMARK);
	if (path == NULL)
		xerr(mainwin.win, EXIT_FAILURE, "g_strdup_printf()");
	if ((fp = fopen(path, "r+")) == NULL && errno == ENOENT) {
		if ((fp = fopen(path, "w+")) == NULL)
			xerr(mainwin.win, EXIT_FAILURE, "fopen(%s)", path);
	} else if (fp == NULL)
		xerr(mainwin.win, EXIT_FAILURE, "fopen(%s)", path);
	if (flock(fileno(fp), LOCK_EX) == -1)
		xerr(mainwin.win, EXIT_FAILURE, "flock()");
	for (found = false; fgets(ln, _POSIX2_LINE_MAX, fp) != NULL;) {
		(void)strtok(ln, "\n\r");
		if (strcmp(ln, bookmark) == 0 ||
		    strcmp(ln + strlen("file://"), bookmark) == 0)
			found = true;
	}
	if (!found)
		(void)fprintf(fp, "file://%s\n", bookmark);
	(void)fclose(fp);
	free(ln);
	g_free(path);
}

/*
 * Deletes any line matching the given string from the ~/.gtk-bookmarks file.
 */
static void
del_bookmark(const char *bookmark)
{
	char	      *path_from, *path_to, *buf, *p;
	FILE	      *fp_from, *fp_to;
	struct passwd *pw;
	
	if ((pw = getpwuid(getuid())) == NULL) {
		xerrx(mainwin.win, EXIT_FAILURE,
		    _("Couldn't find you in the password file."));
	}
	endpwent();
	path_from = g_strdup_printf("%s/%s", pw->pw_dir, PATH_BOOKMARK);
	if (path_from == NULL)
		xerr(mainwin.win, EXIT_FAILURE, "g_strdup_printf()");
	if ((fp_from = fopen(path_from, "r")) == NULL) {
		xwarn(mainwin.win, _("Couldn't open \"%s\""), path_from);
		g_free(path_from);
		return;
	}
	path_to = g_strdup_printf("%s/%s.%d", pw->pw_dir, PATH_BOOKMARK,
	    getpid());
	if (path_to == NULL)
		xerr(mainwin.win, EXIT_FAILURE, "g_strdup_printf()");
	if ((fp_to = fopen(path_to, "w+")) == NULL) {
		xwarn(mainwin.win, _("Couldn't create \"%s\""), path_to);
		g_free(path_from); g_free(path_to);
		(void)fclose(fp_from);
		return;
	}
	if (flock(fileno(fp_from), LOCK_EX) == -1) {
		xwarn(mainwin.win, "flock()");
		(void)fclose(fp_from); (void)fclose(fp_to);
		g_free(path_from); g_free(path_to); 
		return;
	}
	if ((buf = malloc(_POSIX2_LINE_MAX)) == NULL)
		xerr(mainwin.win, EXIT_FAILURE, "malloc()");
	while ((p = fgets(buf, _POSIX2_LINE_MAX, fp_from)) != NULL) {
		(void)strtok(buf, "\n\r");
		while (isspace(*p))
			p++;
		if (*p == '\0')
			continue;
		if (strncmp(p, "file://", 7) == 0)
			p += 7;
		if (strcmp(p, bookmark) != 0)
			(void)fprintf(fp_to, "%s\n", buf);
	}
	if (rename(path_to, path_from) == -1)
		warn("rename(%s, %s)", path_to, path_from);
	g_free(path_from); g_free(path_to);
	(void)fclose(fp_to);
	(void)fclose(fp_from);
	free(buf);
}

static void
call_reply_function()
{
	/* Call reply function of last executed command. */
	cmdq[cmdqidx].re(cmdq[cmdqidx].icon);

	if (++cmdqidx == cmdqlen) {
		/* Queue completely processed. */
		cmdqlen = cmdqidx = 0;
	} else {
		/* Execute next command in queue. */
		(void)fputs(cmdq[cmdqidx].cmd, sock);
	}
}

static void
cb_mount(GtkWidget *widget, gpointer data)
{
	icon_t *icon;

	icon = (icon_t *)data;
	sndcmd(process_mount_reply, icon, "mount %s\n", icon->drvp->dev);
	busywin(BUSYWIN_MSG, true);
}

static void
process_mount_reply(icon_t *icon)
{
	const char *msg;

	busywin(NULL, false);
	switch (dsbmdevent.type) {
	case EVENT_SUCCESS_MSG:
		icon->drvp->mounted = true;
		free(icon->drvp->mntpt);
		icon->drvp->mntpt = strdup(dsbmdevent.drvinfo.mntpt);
		if (icon->drvp->mntpt == NULL)
			xerr(mainwin.win, EXIT_FAILURE, "strdup()");
		set_mounted(icon, true);
		add_bookmark(icon->drvp->mntpt);
		cb_size(NULL, icon);
		return;
	case EVENT_ERROR_MSG:
		if (dsbmdevent.code < 255) {
			errno = dsbmdevent.code;
			xwarn(mainwin.win,
			    _("Mounting failed with the following error"));
		} else {
			if ((msg = errmsg(dsbmdevent.code)) == NULL) {
				xwarnx(mainwin.win,
				    _("Mounting failed with error " \
				    "code %d"), dsbmdevent.code);
			} else
				xwarnx(mainwin.win, msg);
		}
	}
}

static void
cb_cb(GtkWidget *cb, gpointer bp)
{
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cb)))
		*(bool *)bp = true;
	else
		*(bool *)bp = false;
}

static void
cb_unmount(GtkWidget *widget, gpointer data)
{
	icon_t *icon;

	icon = (icon_t *)data;
	sndcmd(process_unmount_reply, icon, "unmount %s\n", icon->drvp->dev);
	busywin(BUSYWIN_MSG, true);
}

static void
process_unmount_reply(icon_t *icon)
{
	const char *msg;

	busywin(NULL, false);
	switch (dsbmdevent.type) {
	case EVENT_SUCCESS_MSG:
		icon->drvp->mounted = false;
		del_bookmark(icon->drvp->mntpt);
		free(icon->drvp->mntpt);
		icon->drvp->mntpt = NULL;
		set_mounted(icon, false);
		return;
	case EVENT_ERROR_MSG:
		if (dsbmdevent.code == ERR_DEVICE_BUSY ||
		    dsbmdevent.code == EBUSY) {
			if (yesnobox(mainwin.win,  _(UNMOUNT_BUSY_MSG)) == 1) {
				sndcmd(process_unmount_reply, icon,
				    "unmount -f %s\n", icon->drvp->dev);
				busywin(BUSYWIN_MSG, true);
			}
			return;
		} else if (dsbmdevent.code < 255) {
			errno = dsbmdevent.code;
			xwarn(mainwin.win,
			    _("Unmounting failed with the " \
			      "following error"));
			return;
		} else {
			msg = errmsg(dsbmdevent.code);
			if (msg == NULL) {
				xwarnx(mainwin.win,
				    _("Unmounting failed with " \
				      "error code %d"), dsbmdevent.code);
			} else
				xwarnx(mainwin.win, msg);
			return;
		}
	}
}

/*
 * Mounts a drive and open the mount point with the user defined filemanager.
 */
static void
cb_open(GtkWidget *widget, gpointer data)
{
	icon_t *icon;

	if (dsbcfg_getval(cfg, CFG_FILEMANAGER).string == NULL ||
	    !*dsbcfg_getval(cfg, CFG_FILEMANAGER).string)
		return;
	icon = (icon_t *)data;
	if (!icon->drvp->mounted) {
		sndcmd(process_open_reply, icon, "mount %s\n",
		    icon->drvp->dev);
		busywin(NULL, false);
		busywin(BUSYWIN_MSG, true);
	} else if (dsbcfg_getval(cfg, CFG_FILEMANAGER).string != NULL) {
		exec_cmd(dsbcfg_getval(cfg, CFG_FILEMANAGER).string,
		    icon->drvp);
	}
}

static void
process_open_reply(icon_t *icon)
{
	const char *msg;

	busywin(NULL, false);
	switch (dsbmdevent.type) {
	case EVENT_SUCCESS_MSG:
		icon->drvp->mounted = true;
		free(icon->drvp->mntpt);
		icon->drvp->mntpt = strdup(dsbmdevent.drvinfo.mntpt);
		if (icon->drvp->mntpt == NULL)
			xerr(mainwin.win, EXIT_FAILURE, "strdup()");
		set_mounted(icon, true);
		add_bookmark(icon->drvp->mntpt);
		cb_size(NULL, icon);
		if (dsbcfg_getval(cfg, CFG_FILEMANAGER).string == NULL)
			return;
		exec_cmd(dsbcfg_getval(cfg, CFG_FILEMANAGER).string,
		    icon->drvp);
		return;
	case EVENT_ERROR_MSG:
		if (dsbmdevent.code < 255) {
			errno = dsbmdevent.code;
			xwarn(mainwin.win,
			    _("Mounting failed with the " \
			      "following error"));
		} else {
			msg = errmsg(dsbmdevent.code);
			if (msg == NULL) {
				xwarnx(mainwin.win,
				    _("Mounting failed with error " \
				      "code %d"), dsbmdevent.code);
			} else
				xwarnx(mainwin.win, msg);
		}
	}
}

static void
cb_size(GtkWidget *widget, gpointer data)
{
	icon_t *icon;

	icon = (icon_t *)data;
	sndcmd(process_size_reply, icon, "size %s\n", icon->drvp->dev);
}

static void
process_size_reply(icon_t *icon)
{
	int	   i;
	gchar	   *str;
	double	   ms, fs;
	u_long	   q, r, div[] = { 1, 1 << 10, 1 << 20, 1 << 30 };
	const char *u_ms, *u_fs, *units[] = { "Bytes", "KB", "MB", "GB" };

	if (dsbmdevent.type == EVENT_SUCCESS_MSG) {
		for (i = 1; i < sizeof(div) / sizeof(u_long); i++) {
			if (dsbmdevent.mediasize < div[i])
				break;
		}
		u_ms = units[i - 1];

		q = dsbmdevent.mediasize / div[i - 1];
		r = dsbmdevent.mediasize - div[i - 1] * q;
		if (q > 0)
			ms = 0.05 + (double)q + (double)r / (double)div[i - 1];
		else
			ms = 0;
		for (i = 1; i < sizeof(div) / sizeof(u_long); i++) {
			if (dsbmdevent.free < div[i])
				break;
		}
		u_fs = units[i - 1];

		q = dsbmdevent.free / div[i - 1];
		r = dsbmdevent.free - div[i - 1] * q;
		fs = (double)q + (double)r / (double)div[i - 1];

		str = g_strdup_printf(
		    _(" %s\tDisk size: %.1f %s\tFree: %.1f %s"),
		     icon->drvp->dev, ms, u_ms, fs, u_fs);
		gtk_statusbar_push(GTK_STATUSBAR(mainwin.statusbar), 0, str);
		g_free(str);
	}
}

static void
cb_speed(GtkWidget *widget, gpointer data)
{
	icon_t	      *icon;
	GtkWidget     *win, *label, *spin, *ok, *cancel;
	static int    speed = 0;
	GtkAdjustment *adj;

	icon = (icon_t *)data;
	win = gtk_dialog_new();
	gtk_window_set_title(GTK_WINDOW(win), _("Set CDROM speed"));
	gtk_window_set_icon_name(GTK_WINDOW(win), "preferences-system");
	gtk_container_set_border_width(GTK_CONTAINER(win), 10);
	adj   = GTK_ADJUSTMENT(gtk_adjustment_new((float)icon->drvp->speed,
	    1, CDR_MAXSPEED, 1.0, 1.0, 0.0));
	spin  = gtk_spin_button_new(adj, 1, 0);
	label = new_pango_label(ALIGN_CENTER, ALIGN_CENTER,
	    "<span font_weight=\"bold\">%s</span>",
	    "Set max. CDROM reading speed\n");
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(win))), label,
	    TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(win))), spin,
	    FALSE, TRUE, 0);
	ok     = gtk_button_new_with_mnemonic(_("_Ok"));
	cancel = gtk_button_new_with_mnemonic(_("_Cancel"));
	gtk_dialog_add_action_widget(GTK_DIALOG(win), ok,
	    GTK_RESPONSE_ACCEPT);
	gtk_dialog_add_action_widget(GTK_DIALOG(win), cancel,
	    GTK_RESPONSE_REJECT);
	gtk_widget_show_all(win);

	if (gtk_dialog_run(GTK_DIALOG(win)) != GTK_RESPONSE_ACCEPT) {
		gtk_widget_destroy(win);
		return;
	}
	speed = (int)gtk_adjustment_get_value(GTK_ADJUSTMENT(adj));
	sndcmd(process_speed_reply, icon, "speed %s %d\n",
	    icon->drvp->dev, speed);
	gtk_widget_destroy(win);
	busywin(BUSYWIN_MSG, true);
}

static void
process_speed_reply(icon_t *icon)
{
	const char *msg;

	busywin(NULL, false);
	switch (dsbmdevent.type) {
	case EVENT_ERROR_MSG:
		if (dsbmdevent.code < 255) {
			errno = dsbmdevent.code;
			xwarn(mainwin.win,
			     _("Setting speed failed with the " \
			       "following error"));
		} else {
			msg = errmsg(dsbmdevent.code);
			if (msg == NULL) {
				xwarnx(mainwin.win,
				    _("Setting speed failed with " \
				      "error code %d"), dsbmdevent.code);
			} else
				xwarnx(mainwin.win, msg);
		}
	case EVENT_SUCCESS_MSG:
		icon->drvp->speed = dsbmdevent.drvinfo.speed;
	}
}

static void
cb_eject(GtkWidget *widget, gpointer data)
{
	icon_t *icon;

	icon = (icon_t *)data;

	sndcmd(process_eject_reply, icon, "eject %s\n", icon->drvp->dev);
	busywin(BUSYWIN_MSG, true);
}

static void
process_eject_reply(icon_t *icon)
{
	const char *msg;

	busywin(NULL, false);
	switch (dsbmdevent.type) {
	case EVENT_ERROR_MSG:
		if (dsbmdevent.code == ERR_DEVICE_BUSY ||
		    dsbmdevent.code == EBUSY) {
			if (yesnobox(mainwin.win, _(EJECT_BUSY_MSG)) == 1) {
				sndcmd(process_eject_reply, icon,
				    "eject -f %s\n", icon->drvp->dev);
				busywin(BUSYWIN_MSG, true);
			} else
				return;
		} else if (dsbmdevent.code < 255) {
			errno = dsbmdevent.code;
			xwarn(mainwin.win, _("Ejecting failed with the " \
			    "following error"));
			return;
		} else {
			msg = errmsg(dsbmdevent.code);
			if (msg == NULL) {
				xwarnx(mainwin.win, _("Ejecting failed with " \
				    "error code %d"), dsbmdevent.code);
				return;
			} else {
				xwarnx(mainwin.win, msg);
				return;
			}
		}
	case EVENT_SUCCESS_MSG:
		if (icon->drvp->mounted)
			del_bookmark(icon->drvp->mntpt);
	}
}

static void
cb_play(GtkWidget *widget, gpointer data)
{
	char   *cmd;
	icon_t *icon;

	icon = (icon_t *)data;

	switch (icon->drvp->type) {
	case DSKTYPE_AUDIOCD:
		cmd = dsbcfg_getval(cfg, CFG_PLAY_CDDA).string;
		break;
	case DSKTYPE_DVD:
		cmd = dsbcfg_getval(cfg, CFG_PLAY_DVD).string;
		break;
	case DSKTYPE_VCD:
		cmd = dsbcfg_getval(cfg, CFG_PLAY_VCD).string;
		break;
	case DSKTYPE_SVCD:
		cmd = dsbcfg_getval(cfg, CFG_PLAY_SVCD).string;
		break;
	default:
		return;
	}
	if (cmd == NULL || *cmd == '\0')
		return;
	exec_cmd(cmd, icon->drvp);
}

static drive_t *
add_drive(drive_t *drvp)
{
	bool hide;
	char **v;

	for (hide = false, v = dsbcfg_getval(cfg, CFG_HIDE).strings;
	    !hide && v != NULL && *v != NULL; v++) {
		if (strcmp(drvp->dev, *v) == 0)
			hide = true;
		else if (drvp->mntpt != NULL && strcmp(drvp->mntpt, *v) == 0)
			hide = true;
	}
	if (hide)
		return (NULL);
	drives = realloc(drives, (ndrives + 1) * sizeof(drive_t *));
	if (drives == NULL || drvp->dev == NULL)
		err(EXIT_FAILURE, "realloc()");
	if ((drives[ndrives] = malloc(sizeof(drive_t))) == NULL)
		err(EXIT_FAILURE, "malloc()");
	drives[ndrives]->dev = strdup(drvp->dev);
	if (drvp->volid != NULL)
		drives[ndrives]->volid = strdup(drvp->volid);
	else
		drives[ndrives]->volid = NULL;
	if (drvp->mntpt != NULL) {
		drives[ndrives]->mntpt   = strdup(drvp->mntpt);
		drives[ndrives]->mounted = true;
	} else {
		drives[ndrives]->mntpt	 = NULL;
		drives[ndrives]->mounted = false;
	}
	drives[ndrives]->speed = drvp->speed;
	drives[ndrives]->type  = drvp->type;
	drives[ndrives]->cmds  = drvp->cmds;

	/* Add our own commands to the device's command list, and set VolIDs. */
	switch (drvp->type) {
	case DSKTYPE_AUDIOCD:
		drives[ndrives]->volid = strdup("Audio CD");
	case DSKTYPE_DVD:
		if (drives[ndrives]->volid == NULL)
			drives[ndrives]->volid = strdup("DVD");
	case DSKTYPE_SVCD:
		if (drives[ndrives]->volid == NULL)
			drives[ndrives]->volid = strdup("SVCD");
	case DSKTYPE_VCD:
		if (drives[ndrives]->volid == NULL)
			drives[ndrives]->volid = strdup("VCD");
		/* Playable media. */
		drives[ndrives]->cmds |= DRVCMD_PLAY;
	}
	if ((drvp->cmds & DRVCMD_MOUNT)) {
		/* Device we can open in a filemanager. */
		drives[ndrives]->cmds |= DRVCMD_OPEN;
	}
	if (drives[ndrives]->volid == NULL)
		drives[ndrives]->volid = strdup(drvp->dev);
	drives[ndrives]->cmds |= (DRVCMD_OPEN | DRVCMD_HIDE);
	return (drives[ndrives++]);
}

static void
del_drive(const char *dev)
{
	int i;

	for (i = 0; i < ndrives; i++) {
		if (strcmp(drives[i]->dev, dev) == 0)
			break;
	}
	if (i == ndrives)
		return;
	free(drives[i]->dev);
	free(drives[i]->volid);
	free(drives[i]->mntpt);
	free(drives[i]);
	for (; i < ndrives - 1; i++)
		drives[i] = drives[i + 1];
	ndrives--;
}

static int
parse_dsbmdevent(char *str)
{
	int  i, len;
	char *p, *q, *tmp;

	/* Init */
	for (i = 0; i < NKEYWORDS; i++) {
		if (dsbmdkeywords[i].val.string != NULL)
			*dsbmdkeywords[i].val.string = NULL;
	}
	for (p = str; (p = strtok(p, ":\n")) != NULL; p = NULL) {
		for (i = 0; i < NKEYWORDS; i++) {
			len = strlen(dsbmdkeywords[i].key);
			if (strncmp(dsbmdkeywords[i].key, p, len) == 0)
				break;
		}
		if (i == NKEYWORDS) {
			warnx("Unknown keyword '%s'", p);
			continue;
		}
		switch (dsbmdkeywords[i].type) {
		case KWTYPE_STRING:
			*dsbmdkeywords[i].val.string = p + len;
			break;
		case KWTYPE_CHAR:
			*dsbmdkeywords[i].val.character = *p;
			break;
		case KWTYPE_INTEGER:
			*dsbmdkeywords[i].val.integer =
			    strtol(p + len, NULL, 10);
			break;
		case KWTYPE_UINT64:
			*dsbmdkeywords[i].val.uint64 =
			    (uint64_t)strtoll(p + len, NULL, 10);
			break;
		case KWTYPE_COMMANDS:
			dsbmdevent.drvinfo.cmds = 0;
			if ((q = tmp = strdup(p + len)) == NULL)
				xerr(NULL, EXIT_FAILURE, "strdup()");
			for (i = 0; (q = strtok(q, ",")) != NULL; q = NULL) {
				for (i = 0; i < NCMDS; i++) {
					if (strcmp(cmdtbl[i].name, q) == 0) {
						dsbmdevent.drvinfo.cmds |=
						    cmdtbl[i].cmd;
					}
				}
			}
			free(tmp);
			break;
		case KWTYPE_DSKTYPE:
			for (i = 0; i < NDSKTYPES; i++) {
				if (strcmp(disktypetbl[i].name, p + len) == 0) {
					dsbmdevent.drvinfo.type =
					    disktypetbl[i].type;
					break;
                        	}
                	}
			break;
		}
	}
	return (0);
}

static int
create_mddev(const char *image)
{
	int  i;
	char *path, *cmd, *p;
	const char *errstr;

	if (sock == NULL) {
		for (i = 0; i < 10 &&
		    (sock = uconnect(PATH_DSBMD_SOCKET)) == NULL; i++) {
			if (errno == EINTR || errno == ECONNREFUSED)
				(void)sleep(1);
			else {
				xerr(NULL, EXIT_FAILURE,
				    "Couldn't connect to DSBMD");
			}
		}
		if (i == 10)
			xerr(NULL, EXIT_FAILURE, "Couldn't connect to DSBMD");
		for (p = readln(true); p[0] != '='; p = readln(true)) {
			if (parse_dsbmdevent(p) == -1)
				continue;
			if (dsbmdevent.type == EVENT_ERROR_MSG &&
			    dsbmdevent.code == ERR_PERMISSION_DENIED) {
				xerrx(NULL, EXIT_FAILURE,
				  _("You are not allowed to connect to DSBMD"));
			} else if (dsbmdevent.type == EVENT_SHUTDOWN) {
				xerrx(NULL, EXIT_FAILURE,
				    _("DSBMD just shut down."));
			}
		}
	}
	if ((path = realpath(image, NULL)) == NULL)
		xwarn(NULL, "realpath(%s)", image);
	if ((cmd = malloc(strlen(path) + strlen("mdattach ") + 8)) == NULL)
		xerr(NULL, EXIT_FAILURE, "malloc()");
	(void)sprintf(cmd, "mdattach \"%s\"\n", path);
	errno = 0;
	while (fputs(cmd, sock) == EOF) {
		if (errno == EINTR)
			continue;
		xerr(NULL, EXIT_FAILURE, "fputs()");
	}
	free(cmd);

	while ((p = readln(true)) != NULL) {
		if (parse_dsbmdevent(p) != 0)
			continue;
		if (dsbmdevent.type == EVENT_ERROR_MSG) {
			errstr = errmsg(dsbmdevent.code);
			xwarnx(NULL, "Couldn't create memory disk from " \
			    "'%s': Error code %d: %s", path, dsbmdevent.code,
			    errstr == NULL ? "" : errstr);
			free(path);
			return (-1);
		}
		if (dsbmdevent.type == EVENT_SUCCESS_MSG) {
			free(path);
			return (0);
		}
	}
	return (-1);
}

static FILE *
uconnect(const char *path)
{
	int  s;
	FILE *sp;
	struct sockaddr_un saddr;

	if ((s = socket(PF_LOCAL, SOCK_STREAM, 0)) == -1)
		return (NULL);
	(void)memset(&saddr, (unsigned char)0, sizeof(saddr));
	(void)snprintf(saddr.sun_path, sizeof(saddr.sun_path), "%s", path);
	saddr.sun_family = AF_LOCAL;
	if (connect(s, (struct sockaddr *)&saddr, sizeof(saddr)) == -1)
		return (NULL);
	if ((sp = fdopen(s, "r+")) == NULL)
		return (NULL);
	/* Make the stream line buffered, and the socket non-blocking. */
	if (setvbuf(sp, NULL, _IOLBF, 0) == -1 ||
	    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK) == -1)
		return (NULL);
	return (sp);
}

