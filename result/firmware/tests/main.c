#include <stdio.h>

int t_failures;
int t_checks;

void suite_core(void);
void suite_storage(void);
void suite_kproto(void);
void suite_system(void);
void suite_features(void);
void suite_cave(void);
void suite_power(void);
void suite_power_cave(void);
void suite_sysex_cave(void);
void suite_bt_cave(void);
void suite_btsrc(void);
void suite_kpsess(void);
void suite_kpgate(void);
void suite_recview(void);
void suite_recctl(void);
void suite_kpstate(void);
void suite_kpmetro(void);
void suite_relay_cave(void);
void suite_recctl_cave(void);
void suite_recsave(void);
void suite_recdel(void);
void suite_kpstart(void);
void suite_xgate(void);
void suite_kpchan(void);
void suite_gpiodiag(void);

int main(void)
{
    suite_core();
    suite_storage();
    suite_kproto();
    suite_system();
    suite_features();
    suite_cave();
    suite_power();
    suite_power_cave();
    suite_sysex_cave();
    suite_bt_cave();
    suite_btsrc();
    suite_kpsess();
    suite_kpgate();
    suite_recview();
    suite_recctl();
    suite_kpstate();
    suite_kpmetro();
    suite_relay_cave();
    suite_recctl_cave();
    suite_recsave();
    suite_recdel();
    suite_kpstart();
    suite_xgate();
    suite_kpchan();
    suite_gpiodiag();
    printf("\n%d checks, %d failures\n", t_checks, t_failures);
    return t_failures ? 1 : 0;
}
