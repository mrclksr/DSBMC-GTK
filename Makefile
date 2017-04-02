PROGRAM		     = dsbmc
PREFIX	    	    ?= /usr/local
BINDIR		     = ${PREFIX}/bin
LOCALEDIR	     = ${PREFIX}/share/locale
CFLAGS		    += -Wall `pkg-config gtk+-2.0 --cflags --libs`
CFLAGS		    += -DPROGRAM=\"${PROGRAM}\" -DPATH_LOCALE=\"${LOCALEDIR}\"
TARGETS		     = ${PROGRAM}
SOURCES		     = ${PROGRAM}.c gtk-helper/gtk-helper.c dsbcfg/dsbcfg.c
NLS_LANGS	     = de
NLS_SOURCES	     = ${NLS_TARGETS:R:S,$,.po,}
NLS_TARGETS	     = ${NLS_LANGS:S,^,locale/,:S,$,.mo,}
BSD_INSTALL_DATA    ?= install -m 0644
BSD_INSTALL_PROGRAM ?= install -s -m 555

.if !defined(WITHOUT_GETTEXT)
CFLAGS  += -DWITH_GETTEXT
TARGETS += ${NLS_TARGETS}
.endif

all: ${TARGETS}

${PROGRAM}: ${SOURCES}
	${CC} -o ${PROGRAM} ${CFLAGS} ${SOURCES}

${NLS_TARGETS}: ${NLS_SOURCES}
	for i in locale/*.po; do \
		msgfmt -c -v -o $${i%po}mo $$i; \
	done

install: ${TARGETS}
	${BSD_INSTALL_PROGRAM} ${PROGRAM} ${DESTDIR}${BINDIR}
	${BSD_INSTALL_DATA} ${PROGRAM}.desktop \
		${DESTDIR}${PREFIX}/share/applications

.if !defined(WITHOUT_GETTEXT)
	(cd locale && for i in *.mo; do \
		${BSD_INSTALL_DATA} $$i \
		${DESTDIR}${LOCALEDIR}/$${i%.mo}/LC_MESSAGES/${PROGRAM}.mo; \
	done)
.endif

clean:
	-rm -f ${PROGRAM}
	-rm -f locale/*.mo

