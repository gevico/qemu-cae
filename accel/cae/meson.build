cae_specific_ss = ss.source_set()
cae_specific_ss.add(files(
  'cae-all.c',
))
specific_ss.add_all(when: 'CONFIG_CAE', if_true: cae_specific_ss)

cae_system_ss = ss.source_set()
cae_system_ss.add(when: ['CONFIG_SYSTEM_ONLY', 'CONFIG_CAE'], if_true: files(
  'cae-accel-ops.c',
  'cae-accel-ops-rr.c',
))
system_ss.add_all(cae_system_ss)
