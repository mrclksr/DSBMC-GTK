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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <err.h>

#include "gtk-helper.h"

extern int errno;

/* Message buffer for xwarn* and xerr* */
static char msgbuf[1024];

#ifdef WITH_GETTEXT
char *
gettext_wrapper(const char *str)
{
	int   _errno;
	gchar *p;
	gsize wrt;
	
	_errno = errno;
	p = g_locale_to_utf8(gettext(str), -1, NULL, &wrt, NULL);
	if (p == NULL)
		return ((char *)str);
	errno = _errno;
	return (p);
}
#endif

GdkPixbuf *
load_icon(int size, const char *name, ...)
{
	va_list	     ap;
	GdkPixbuf    *icon;
	const char   *s;
	GtkIconTheme *icon_theme;

	va_start(ap, name);
	icon_theme = gtk_icon_theme_get_default();

	for (s = name; s != NULL || (s = va_arg(ap, char *)); s = NULL) {
		if ((icon = gtk_icon_theme_load_icon(icon_theme, s, size, 0,
		    NULL)) != NULL)
			return (icon);
	}
	return (NULL);
}

static void
infobox(GtkWindow *parent, const char *str, const GtkMessageType type)
{
	GtkWidget    *dialog;

	dialog = gtk_message_dialog_new(parent,
                    GTK_DIALOG_DESTROY_WITH_PARENT, type,
                    GTK_BUTTONS_OK, "%s", str);
	gtk_dialog_run(GTK_DIALOG (dialog));
	gtk_widget_destroy(dialog);
}

void
xerr(GtkWindow *parent, int eval, const char *fmt, ...)
{
	int	_errno;
	gchar	*p;
	gsize	wrt;
	size_t	len, rem;
	va_list	ap;

	_errno = errno;
	
	msgbuf[0] = '\0';

	va_start(ap, fmt);
	(void)vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
	len = strlen(msgbuf);
	rem = sizeof(msgbuf) - len - 1;

	p = g_locale_to_utf8(strerror(_errno), -1, NULL, &wrt, NULL);
	if (p == NULL)
		p = strerror(_errno);
	(void)snprintf(msgbuf + len, rem, ":\n%s\n", p);
	infobox(parent, msgbuf, GTK_MESSAGE_ERROR);
	exit(eval);
}

void
xerrx(GtkWindow *parent, int eval, const char *fmt, ...)
{
	va_list	ap;

	msgbuf[0] = '\0';
	va_start(ap, fmt);
	(void)vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
	infobox(parent, msgbuf, GTK_MESSAGE_ERROR);
	exit(eval);
}

void
xwarn(GtkWindow *parent, const char *fmt, ...)
{
	int	_errno;
	gchar	*p;
	gsize	wrt;
	size_t	len, rem;
	va_list	ap;

	_errno = errno;
	msgbuf[0] = '\0';

	va_start(ap, fmt);
	(void)vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
	len = strlen(msgbuf);
	rem = sizeof(msgbuf) - len;
	p = g_locale_to_utf8(strerror(_errno), -1, NULL, &wrt, NULL);
	if (p == NULL)
		p = strerror(_errno);
	(void)snprintf(msgbuf + len, rem, ":\n%s\n", p);
	infobox(parent, msgbuf, GTK_MESSAGE_ERROR);
}

void
xwarnx(GtkWindow *parent, const char *fmt, ...)
{
	va_list ap;

	msgbuf[0] = '\0';
	va_start(ap, fmt);
	(void)vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
	infobox(parent, msgbuf, GTK_MESSAGE_WARNING);
}

GtkWidget *
new_label(float xalign, float yalign, const char *str, ...)
{
	char	  *p;
	va_list	  ap;
	GtkWidget *label;

	va_start(ap, str);
	p = g_strdup_vprintf(str, ap);
	label = gtk_label_new(p);
	g_object_set(label, "xalign", xalign, "yalign", yalign, NULL);
	gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_FILL);
	//gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	g_free(p);

	return (label);
}

GtkWidget *
new_pango_label(float xalign, float yalign, const char *markup, ...)
{
	char	  *p;
	va_list	  ap;
	GtkWidget *label;

	va_start(ap, markup);
	p = g_strdup_vprintf(markup, ap);
	label = new_label(xalign, yalign, "");
	gtk_label_set_markup(GTK_LABEL(label), p);
	g_free(p);

	return (label);
}

int
yesnobox(GtkWindow *parent, const char *fmt, ...)
{
	char      *str;
	va_list   ap;
	GtkWidget *hbox, *dialog, *content, *ok, *cancel, *image, *label;
	
	gdk_threads_enter();
	va_start(ap, fmt);
	if ((str = g_strdup_vprintf(fmt, ap)) == NULL)
		xerr(NULL, EXIT_FAILURE, "g_strdup_vprintf()");
	(void)vsprintf(str, fmt, ap);
	ok	= gtk_button_new_with_mnemonic("_Yes");
#if GTK_MAJOR_VERSION < 3
	hbox	= gtk_hbox_new(FALSE, 5);
#else
	hbox	= gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
#endif
	image	= gtk_image_new_from_icon_name("dialog-question",
		      GTK_ICON_SIZE_DIALOG);
	label	= new_pango_label(ALIGN_CENTER, ALIGN_CENTER, str);
	dialog	= gtk_dialog_new();
	cancel	= gtk_button_new_with_mnemonic("_No");
	content	= gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	g_free(str);
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	if (parent != NULL)
		gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
	else
		gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
	gtk_container_set_border_width(GTK_CONTAINER(hbox), 5);
	gtk_container_set_border_width(GTK_CONTAINER(dialog), 5);

	g_object_set(image, "halign", 0, "valign", ALIGN_CENTER, NULL);
	gtk_container_add(GTK_CONTAINER(hbox), image);

	gtk_container_add(GTK_CONTAINER(hbox), label);

	gtk_container_add(GTK_CONTAINER(content), hbox);
	
	gtk_dialog_add_action_widget(GTK_DIALOG(dialog), ok,
	    GTK_RESPONSE_ACCEPT);
	gtk_dialog_add_action_widget(GTK_DIALOG(dialog), cancel,
	    GTK_RESPONSE_REJECT);
	gtk_widget_show_all(dialog);
	
	switch (gtk_dialog_run(GTK_DIALOG(dialog))) {
	case GTK_RESPONSE_ACCEPT:
		gtk_widget_destroy(dialog);
		gdk_threads_leave();
		return (1);
	case GTK_RESPONSE_REJECT:
		gtk_widget_destroy(dialog);
		gdk_threads_leave();
		return (0);
	}
	gtk_widget_destroy(dialog);
	gdk_threads_leave();

	return (-1);
}

