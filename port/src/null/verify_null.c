/*
 * The data check left out (the Dreamcast build): port/src/plat_verify.c compares every data file with the original
 * disc's (a PC install can be modified) and plat_fprint.c computes the gameplay fingerprint (for online play). The
 * Dreamcast's disc is the one the port builds, and it has no online play, so neither runs: no tables in the program,
 * no reading of the whole disc at start. Answered as a run in which the check is off.
 */
void Port_VerifyStart(void) {}
int Port_DataState(void) { return 3; } /* DATA_OFF */
const char *Port_DataSummary(void) { return ""; }

/* plat_fprint.c: -1 = not computed in this run */
int Port_DataFingerprint(unsigned long long *out) { (void)out; return -1; }
int Port_DataFingerprintPart(int part, unsigned long long *out) { (void)part; (void)out; return -1; }
int Port_DataFingerprintOriginal(void) { return -1; }
const char *Port_DataFingerprintText(void) { return ""; }
void PortFp_Run(int detail) { (void)detail; }
