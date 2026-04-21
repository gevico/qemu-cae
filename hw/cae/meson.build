cae_hw_ss = ss.source_set()
cae_hw_ss.add(files(
  'cache.c',
  'cache_mshr.c',
  'dram.c',
  'cpu_inorder.c',
  'cpu_ooo.c',
))

subdir('bpred')
subdir('ooo')

system_ss.add_all(when: 'CONFIG_CAE', if_true: cae_hw_ss)
