/*
 * QRX ABI Startup Isolation 00
 * Absolutely minimal C program. No headers and no explicit OS/library calls.
 * If this crashes at PC 0x00000008, the problem is before application logic:
 * CRT/startup object/linkage/runtime ABI.
 */
int main(void)
{
    return 0;
}
