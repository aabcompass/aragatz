#ifndef ARAGATZ_HW_TESTS_H
#define ARAGATZ_HW_TESTS_H

int aragatz_test_registers(void);
int aragatz_test_smoke(void);
int aragatz_test_pretrigger(void);
int aragatz_test_fragments(void);
int aragatz_test_boundaries(void);
int aragatz_test_stop_restart(void);
int aragatz_test_recovery(void);
int aragatz_test_long(void);
int aragatz_test_ddr(void);
int aragatz_test_repeat(void);
int aragatz_test_endurance(void);
int aragatz_test_rate_sweep(void);
int aragatz_test_all(void);

/* Shared preconditions for the reserved-buffer memory test. */
int aragatz_check_buffer(void);
int aragatz_prepare_idle(void);

#endif
