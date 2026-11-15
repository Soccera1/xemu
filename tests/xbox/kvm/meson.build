if 'CONFIG_KVM' in config_all_accel and host_os == 'linux' and host_machine.cpu_family() in ['x86', 'x86_64']
  test_xbox_kvm = executable(
    'test-xbox-kvm',
    files('test-xbox-kvm.c', '../../../hw/xbox/kvm.c'),
    c_args: [
      '-DCOMPILING_PER_TARGET',
      '-DCONFIG_TARGET="i386-softmmu-config-target.h"',
      '-DCONFIG_DEVICES="i386-softmmu-config-devices.h"',
    ],
    include_directories: [include_directories('../../../target/i386')],
    dependencies: [qemuutil],
    build_by_default: false,
  )
  test('test-xbox-kvm', test_xbox_kvm, suite: ['xbox'])
endif
