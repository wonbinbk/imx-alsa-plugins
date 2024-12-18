if FSL_USE_SWPDM
asound_module_pcm_cicFilter_LTLIBRARIES = libasound_module_pcm_cicFilter.la

asound_module_pcm_cicFilterdir = @ALSA_PLUGIN_DIR@

AM_CFLAGS = -Wall -g @ALSA_CFLAGS@
AM_LDFLAGS = -module -avoid-version -export-dynamic -no-undefined $(LDFLAGS_NOUNDEFINED)

libasound_module_pcm_cicFilter_la_SOURCES = swpdm.c
libasound_module_pcm_cicFilter_la_LIBADD = @ALSA_LIBS@ -limxswpdm -lstdc++ -lm

install-data-hook:
	mkdir -p $(DESTDIR)@ALSA_PLUGIN_DIR@
	rm -f $(DESTDIR)@ALSA_PLUGIN_DIR@/libasound_module_pcm_cicFilter_*.so
	$(LN_S) libasound_module_pcm_cicFilter.so $(DESTDIR)@ALSA_PLUGIN_DIR@/libasound_module_pcm_cicFilter_fast.so

uninstall-hook:
	rm -f $(DESTDIR)@ALSA_PLUGIN_DIR@/libasound_module_pcm_cicFilter_*.so
endif
