# Folder of the Janus installation prefix (we add /include/janus for the headers)
JANUSP ?= /opt/janus

CFGDIR = conf
BLDDIR = build

SOURCE = src/janus_ndi.c
TARGET = janus_ndi.so
CFGFILE = janus.plugin.ndi.jcfg.sample

CFLAGS += -I$(JANUSP)/include $(shell pkg-config --cflags glib-2.0 jansson opus libcurl) -D_GNU_SOURCE -DHAVE_SRTP_2
LDFLAGS += $(shell pkg-config --libs glib-2.0 jansson opus libcurl)

JCFLAGS = -g -O2 -fstack-protector -Wall -Wextra -Wformat=2 -Wpointer-arith \
          -Wstrict-prototypes -Wredundant-decls -Wwrite-strings \
          -Waggregate-return -Wlarger-than=65536 -Winline -Wpacked \
          -Winit-self -Wno-unused-parameter -Wno-missing-field-initializers \
          -Wno-override-init

# Uncomment if you want to build with libasan (for debugging leaks)
ASAN = -O1 -g3 -ggdb3 -fno-omit-frame-pointer -fsanitize=address -fno-sanitize-recover=all -fsanitize-address-use-after-scope
ASAN_LIBS = -fsanitize=address

# Copy the NDI includes and shared objects to the right place
NDI = -I/usr/include/NDI
NDI_LIBS = -lndi
LIBAV = $(shell pkg-config --cflags libavutil libavcodec libavformat libswscale libswresample)
LIBAV_LIBS = $(shell pkg-config --libs libavutil libavcodec libavformat libswscale libswresample)

all: $(BLDDIR)/$(TARGET) $(BLDDIR)/$(TOOL)

demo: $(BLDDIR)/$(DEMO)

$(BLDDIR)/$(TARGET): $(SOURCE)
	@mkdir -p $(dir $@)
	$(CC) -fPIC -shared -o $@ $< $(JCFLAGS) $(CFLAGS) $(ASAN) $(NDI) $(LIBAV) $(LDFLAGS) -ldl -rdynamic $(ASAN_LIBS) $(NDI_LIBS) $(LIBAV_LIBS)

clean:
	rm -rf $(BLDDIR)

install: all
	rm -f $(JANUSP)/lib/janus/plugins/janus_ndi.so
	install $(BLDDIR)/$(TARGET) $(JANUSP)/lib/janus/plugins/
	install -m 0644 $(CFGDIR)/$(CFGFILE) $(JANUSP)/etc/janus/

.PHONY: all install clean
