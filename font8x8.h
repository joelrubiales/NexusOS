// Fuente 8x8 básica para ASCII imprimible + extras (NexusOS)
#ifndef FONT8X8_H
#define FONT8X8_H

// Devuelve puntero a 8 bytes (1 byte por fila, MSB->izquierda).
const unsigned char* font8x8_get(unsigned char ch);

#endif
