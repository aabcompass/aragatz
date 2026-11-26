#ifndef TEST_CAPTURE_XPSEUDO_ASM_H
#define TEST_CAPTURE_XPSEUDO_ASM_H
void test_capture_dsb(void);
#define dsb() test_capture_dsb()
#endif
