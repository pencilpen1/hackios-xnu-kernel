/* Run the public Metal device/command test before enabling discovery for iOS. */
static int
lancelot_metal_prepare(void)
{
    const char *mode = boot_arg("metal");
    if (!mode || strcmp(mode, "1")) return 1;
    const char *ca = boot_arg("camtl");
    const char *stage = ca && !strcmp(ca, "1") ? "--ca" : "--basic";
    const char *program = "/redwood/bin/lancelot_metal_test";
    char *arguments[] = { (char *)program, (char *)stage, NULL };
    char *environment[] = { "PATH=/bin:/sbin:/usr/bin:/usr/sbin", "HOME=/var/mobile", NULL };
    pid_t pid = fork();
    if (pid == 0) {
        execve(program, arguments, environment);
        _exit(127);
    }
    if (pid < 0) {
        printf("lancelot-metal-boot: failed=1 fork_errno=%d; discovery stays off\n", errno);
        return 0;
    }
    int status = 0;
    pid_t waited;
    do { waited = wait4(pid, &status, 0, NULL); } while (waited < 0 && errno == 4);
    if (pid <= 0 || waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("lancelot-metal-boot: failed=1 stage=%s child=%d status=%d; discovery stays off\n",
               stage, (int)pid, status);
        return 0;
    }
    printf("lancelot-metal-boot: passed=1 stage=%s device=MTL commands=verified discovery=on\n", stage);
    return 1;
}
