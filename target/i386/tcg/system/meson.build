i386_system_ss.add(when: ['CONFIG_TCG', 'CONFIG_SYSTEM_ONLY'], if_true: files(
  'tcg-cpu.c',
  'smm_helper.c',
  'excp_helper.c',
  'bpt_helper.c',
  'misc_helper.c',
  'fpu_helper.c',
  'svm_helper.c',
  'seg_helper.c',
))
if hvdbt.found()
  # 64-bit long-mode state only: qemu-system-i386 has no lstar/cstar/fmask
  i386_system_ss.add(when: 'TARGET_X86_64', if_true: [files('hvdbt.c'), hvdbt])
endif
