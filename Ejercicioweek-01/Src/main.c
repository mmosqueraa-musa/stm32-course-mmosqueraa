/**
Maria de los Angeles Mosquera
 */
//1. Includes librerias que se van a usar//
#include <stdint.h> 

//2. Definicion de variables gobales//
uint8_t numero1= 10;
uint8_t numero2=3;
char operacion =  '*'; // puede ser '+', '-', '*' o '/'//
uint8_t resultado=0;

//3.Cabeceras (prototipos de funciones)//

uint8_t sumar(uint8_t a, uint8_t b);
uint8_t restar(uint8_t a, uint8_t b);
uint8_t multiplicar(uint8_t a, uint8_t b);
uint8_t dividir(uint8_t a, uint8_t b);

//4. maincon su while(1) //

// Siempre se llama de estas forma
int main(void){
    //Lo que solo se corre una vez//

    /* Loop forever */ // Lo que es 0 es falso y lo que es diferente de 0 es verdadero
	while(1){ 
        if (operacion == '+')
        {resultado = sumar(numero1, numero2);}
        else if (operacion == '-')
        {resultado = restar(numero1, numero2);}
        else if (operacion == '*')
        {resultado = multiplicar(numero1, numero2);}
        else if (dividir == '/'){
            if(numero2 != 0){
                resultado = dividir(numero1, numero2);

            }
        }
        
    }
}


uint8_t sumar(uint8_t a, uint8_t b)
{
return a + b;
}
uint8_t restar(uint8_t a, uint8_t b)
{
return a - b;
}
uint8_t multiplicar(uint8_t a, uint8_t b)
{
return a * b;
}
uint8_t dividir(uint8_t a, uint8_t b)
{
return a / b;
}
