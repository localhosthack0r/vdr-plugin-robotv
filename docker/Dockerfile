FROM alpine:3.12 AS robotv-build
MAINTAINER Alexander pipelka <alexander.pipelka@gmail.com>

ARG VDR_VERSION=2.4

USER root

RUN apk update && apk add build-base freetype-dev fontconfig-dev gettext-dev \
	libjpeg-turbo-dev libcap-dev pugixml-dev curl-dev git bzip2 libexecinfo-dev \
	ncurses-dev bash avahi-libs avahi-dev pcre-dev sqlite-dev pkgconf

RUN mkdir -p /build
WORKDIR /build

RUN echo "building roboTV ..."

RUN git clone git://git.tvdr.de/vdr.git -b stable/${VDR_VERSION} vdr
RUN git clone --single-branch -b v2.2.5 https://github.com/manio/vdr-plugin-dvbapi.git vdr/PLUGINS/src/dvbapi
RUN git clone https://github.com/vdr-projects/vdr-plugin-epgsearch.git vdr/PLUGINS/src/epgsearch
RUN git clone --single-branch -b v2.4.0 https://github.com/rofafor/vdr-plugin-satip.git vdr/PLUGINS/src/satip
RUN git clone https://github.com/vdr-projects/vdr-plugin-streamdev.git vdr/PLUGINS/src/streamdev

# COPY ROBOTV PLUGIN SOURCES
RUN mkdir -p vdr/PLUGINS/src/robotv
COPY . vdr/PLUGINS/src/robotv/
RUN make -C vdr/PLUGINS/src/robotv clean

WORKDIR vdr
COPY docker/templates/Make.* /build/vdr/

RUN mkdir -p /build/patches
COPY docker/patches/ /build/patches/

# VDR patches
RUN for patch in `ls /build/patches/vdr`; do \
        echo ${patch} ; \
        patch -p1 < /build/patches/vdr/${patch} ; \
    done

# satip patches
RUN patch -d PLUGINS/src/satip -p1 < /build/patches/satip/disable-server-timeout.patch

RUN make LIBSQLITE=1 -j 8
RUN mkdir -p /opt/vdr

RUN make LIBSQLITE=1 install

RUN for plugin in robotv epgsearch satip streamdev-server ; do \
        strip -s --strip-debug /opt/vdr/lib/libvdr-${plugin}.so.* ; \
    done ; \
    strip -s --strip-debug /opt/vdr/bin/vdr


RUN rm -Rf /opt/vdr/man
RUN rm -Rf /opt/vdr/locale/*

ENV LIBS="dvbhddevice dvbsddevice epgtableid0 hello osddemo pictures rcu skincurses status svccli svcsvr svdrpdemo"
RUN for lib in ${LIBS} ; do \
    echo "removing /opt/vdr/lib/libvdr-$lib" ; \
        rm -f /opt/vdr/lib/libvdr-${lib}* ; \
    done

FROM alpine:3.12 AS robotv

USER root

ENV DVBAPI_ENABLE="0" \
    DVBAPI_HOST="127.0.0.1" \
    DVBAPI_PORT="2000" \
    DVBAPI_OFFSET=0 \
    STREAMDEV_CLIENT_ENABLE=0 \
    STREAMDEV_CLIENT_PORT=2004 \
    SATIP_ENABLE="0" \
    SATIP_NUMDEVICES="2" \
    SATIP_SERVER="" \
    SATIP_ENABLEEITSCAN="1" \
    ROBOTV_TIMESHIFTDIR="/video" \
    ROBOTV_MAXTIMESHIFTSIZE="4000000000" \
    ROBOTV_PICONSURL= \
    ROBOTV_SERIESFOLDER="Serien" \
    ROBOTV_CHANNELCACHE="true" \
    ROBOTV_EPGIMAGEURL= \
    VDR_LOGLEVEL=2 \
    VDR_UPDATECHANNELS=3 \
    VDR_DISEQC=0 \
    VDR_EPGSCANTYPE=1 \
    VDR_EPGSCANTIMEOUT=5 \
    TZ="Europe/Vienna"

RUN apk update && apk add freetype fontconfig libintl libexecinfo \
    libjpeg-turbo libcap pugixml libcurl avahi-libs sqlite-libs pcre tzdata

RUN mkdir -p /opt && \
    mkdir -p /data && \
    mkdir -p /video && \
    mkdir -p /opt/templates && \
    mkdir -p /timeshift

COPY --from=robotv-build /opt/ /opt/
COPY docker/bin/runvdr.sh /opt/vdr/
COPY docker/templates/diseqc.conf /opt/templates/
COPY docker/templates/scr.conf /opt/templates/
COPY docker/templates/sources.conf /opt/templates/
COPY docker/templates/channels.conf /opt/templates/

RUN chmod +x /opt/vdr/runvdr.sh

ENTRYPOINT [ "/opt/vdr/runvdr.sh" ]
