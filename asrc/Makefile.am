asound_module_rate_asrcrate_LTLIBRARIES = libasound_module_rate_asrcrate.la

asound_module_rate_asrcratedir = @ALSA_PLUGIN_DIR@

AM_CFLAGS = -Wall -g @ALSA_CFLAGS@
AM_LDFLAGS = -module -avoid-version -export-dynamic -no-undefined $(LDFLAGS_NOUNDEFINED)

libasound_module_rate_asrcrate_la_SOURCES = rate_asrcrate.c asrc_pair.c
libasound_module_rate_asrcrate_la_LIBADD = @ALSA_LIBS@

install-data-hook:
	mkdir -p $(DESTDIR)@ALSA_PLUGIN_DIR@
	rm -f $(DESTDIR)@ALSA_PLUGIN_DIR@/libasound_module_rate_asrcrate_*.so
	$(LN_S) libasound_module_rate_asrcrate.so $(DESTDIR)@ALSA_PLUGIN_DIR@/libasound_module_rate_asrcrate_fast.so

uninstall-hook:
	rm -f $(DESTDIR)@ALSA_PLUGIN_DIR@/libasound_module_rate_asrcrate_*.so

noinst_HEADERS = asrc_pair.h
