/* Observed against native x86/x64 regsvr32, using regsvr_fixture.c only. */
static const struct regsvr_case regsvr_cases[] = {
    {(const WORD *)L"/s regfixture.dll","","",0,"A;R:80010106;D;"}, /* register */
    {(const WORD *)L"regfixture.dll /s","","",0,"A;R:80010106;D;"}, /* flag_after */
    {(const WORD *)L"/S /U regfixture.dll","","",0,"A;U:80010106;D;"}, /* unregister */
    {(const WORD *)L"/s /i:alpha regfixture.dll","","",0,"A;R:80010106;I:80010106:0061006c007000680061;D;"}, /* install */
    {(const WORD *)L"/s /u /i:beta regfixture.dll","","",0,"A;X:80010106:0062006500740061;U:80010106;D;"}, /* uninstall */
    {(const WORD *)L"/s /n /i:\"two words\" regfixture.dll","","",0,"A;I:80010106:00740077006f00200077006f007200640073;D;"}, /* install_only */
    {(const WORD *)L"/s /i regfixture.dll","","",0,"A;R:80010106;I:80010106:;D;"}, /* install_empty */
    {(const WORD *)L"/s /n /i: regfixture.dll","","",0,"A;I:80010106:;D;"}, /* install_empty_colon */
    {(const WORD *)L"/s /n regfixture.dll","","",1,""}, /* invalid_n */
    {(const WORD *)L"/s /q regfixture.dll","","",1,""}, /* invalid_option */
    {(const WORD *)L"/s","","",1,""}, /* missing_argument */
    {(const WORD *)L"/s absent-fixture.dll","","",3,""}, /* missing_file */
    {(const WORD *)L"/s regmissing.dll","","",4,"A;D;"}, /* missing_register */
    {(const WORD *)L"/s /u regmissing.dll","","",4,"A;D;"}, /* missing_unregister */
    {(const WORD *)L"/s /n /i regmissing.dll","","",4,"A;D;"}, /* missing_install */
    {(const WORD *)L"/s /i:alpha regfixture.dll","R","",5,"A;R:80010106;D;"}, /* fail_register */
    {(const WORD *)L"/s /i:alpha regfixture.dll","I","",5,"A;R:80010106;I:80010106:0061006c007000680061;D;"}, /* fail_install */
    {(const WORD *)L"/s /u /i:beta regfixture.dll","X","",5,"A;X:80010106:0062006500740061;D;"}, /* fail_uninstall */
    {(const WORD *)L"/s /u /i:beta regfixture.dll","U","",5,"A;X:80010106:0062006500740061;U:80010106;D;"}, /* fail_unregister */
    {(const WORD *)L"/s /i:test regfixture.dll","","1",0,"A;R:80010106;I:80010106:0074006500730074;D;"}, /* s_false */
    {(const WORD *)L"/s regfixture.dll regfixture.dll","","",0,"A;R:80010106;D;A;R:80010106;D;"}, /* duplicate */
    {(const WORD *)L"/s regmissing.dll regfixture.dll","","",4,"A;D;A;R:80010106;D;"}, /* continue_export */
    {(const WORD *)L"/s absent-fixture.dll regfixture.dll","","",3,""}, /* continue_load */
    {(const WORD *)L"/s /su regfixture.dll","","",0,"A;R:80010106;D;"}, /* combined */
    {(const WORD *)L"-s -u regfixture.dll","","",0,"A;U:80010106;D;"}, /* hyphens */
    {(const WORD *)L"/s /c regfixture.dll","","",0,"A;R:80010106;D;"}, /* console_flag */
    {(const WORD *)L"/s /u /n /i:bye regfixture.dll","","",0,"A;X:80010106:006200790065;D;"}, /* uninstall_only */
    {(const WORD *)L"/s \"reg fixture.dll\"","","",0,"A;R:80010106;D;"}, /* spaces */
    {(const WORD *)L"/s /n /i:\"ni\u00f1o \u03a9\" regfixture.dll","","",0,"A;I:80010106:006e006900f1006f002003a9;D;"}, /* unicode */
    {(const WORD *)L"/s /n /i:first /i:second regfixture.dll","","",0,"A;I:80010106:007300650063006f006e0064;D;"}, /* repeat_i */
    {(const WORD *)L"/s /unlikely regfixture.dll","","",0,"A;U:80010106;D;"}, /* u_suffix */
    {(const WORD *)L"/s /never /i:test regfixture.dll","","",0,"A;I:80010106:0074006500730074;D;"}, /* n_suffix */
    {(const WORD *)L"/s /n /invalid regfixture.dll","","",0,"A;I:80010106:;D;"}, /* i_suffix */
    {(const WORD *)L"/s regmissing.dll absent-fixture.dll","","",3,"A;D;"}, /* last_error */
};
