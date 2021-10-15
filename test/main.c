#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>

#include "inpt.h"
#include "debug.h"

void on_button(int idx, int flags);
void on_value(int idx, int amount);

int main(int arc, char** argv) {
    printf("inpt version %s\n", inpt_version());

    inpt_update();

    // pthread_t inpt_thread = {0};
    // pthread_create(&inpt_thread, NULL, (void*(*)(void*))inpt_start, NULL);

    // printf("device count: %i\n", inpt_hid_count());
    char** names = inpt_hid_list();
    puts("select a device:");
    for(int i = 0; i < inpt_hid_count(); i++) {
        printf("\t%i: %s", i, names[i]);
    }
    puts("\n");

    char cin[256];
    gets(cin);
    int selection = atoi(cin);

    inpt_hid_select(selection);

    inpt_hid_on_btn(on_button);
    inpt_hid_on_val(on_value);

    while(1) {
        DEBUG_TIME(inpt_update());
        if(debug.last_ms > 50.0) {
            exit(0);
        }
        // usleep(50);
    }

    return 0;
}

void on_button(int idx, int flags) {
    printf("button[%i] set to %i\n", idx, flags);
}

void on_value(int idx, int amount) {
    printf("value[%i] is changed\n", idx);
}