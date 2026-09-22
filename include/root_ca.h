// File: include/root_ca.h
#ifndef ROOT_CA_H
#define ROOT_CA_H

/**
 * REFACTOR: Deklarasikan variabel sebagai 'extern' untuk mencegah multiple definition.
 * 'extern' memberitahu kompiler bahwa variabel ini ada, tetapi definisinya
 * (alokasi memori dan isinya) berada di file .cpp lain (MyNetworkManager.cpp).
 */
extern const char* root_ca_cert;

#endif // ROOT_CA_H