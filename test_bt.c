#include <stdio.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
int main() {
    int dev_id = hci_get_route(NULL);
    printf(\
