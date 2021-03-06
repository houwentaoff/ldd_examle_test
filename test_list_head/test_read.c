#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

int main()
{
    int fd;
    char str[100]={0};
    int ret;
    int count = 0;

    fd = open("/dev/housir_major0", O_RDONLY);
    if (fd < 0) {
        printf("open file failed\n");
        exit(1);
    }

    while (1) {
        memset(str, 0, sizeof(str));
        ret = read(fd, str, sizeof(str));
        if (ret < 0) {
            printf("read event error!\n");
            exit(1);
        }
        if (ret == 0)
        {
            continue;
        }
        if (0 == (count % 1000))
        {
            printf("userspace[%s]len[%d]\n", str, ret);
        }
        count++;
    }

    return 0;
}

