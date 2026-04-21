cae_ss = ss.source_set()
cae_ss.add(files(
  'engine.c',
  'event.c',
  'uop.c',
  'pipeline.c',
  'mem.c',
  'cpu_model.c',
  'trace-emit.c',
  'checkpoint.c',
))
system_ss.add_all(when: 'CONFIG_CAE', if_true: cae_ss)
