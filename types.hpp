// #include <soci/soci.h>
// #include <soci/oracle/soci-oracle.h>
#ifndef types_h
#define types_h

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#define PRECIO_CLEARING(precio_compra_max, precio_venta_min)                   \
  ((precio_compra_max + precio_venta_min) / 2.0)

struct Orden {
  int idOrden;
  bool esCompra; // true=compra, false=venta
  int idNodo;
  double precio;
  double kwh;
  uint64_t secuencia; // timestamp o contador FIFO
};
// Hora
//  Precio base kWh
// 10 1.00
// 12 1.50
// 14 1.00
// 15 1.20
// 18 2.00
/*
NodoRed (abstracta):
  - Atributos:
      int id,
      std::string ubicacion,
      double bal­anceEnergia (kW actual),
      double saldoCuenta (créditos).
  - Métodos virtuales
      puros:
        virtual double calcularExcedente() = 0;
*/

class NodoRed {
private:
  int id;
  std::string ubicacion;
  double balanceEnergia; // kW
  double saldoCuenta;

public:
  virtual double calcularExcedente() = 0;
  virtual ~NodoRed() = default;
};

/*
NodoConsumidor (hereda de NodoRed):
  - Atributo adicional:
      PerfilConsumo perfil (enum Residencial, Comercial, Industrial).

  - calcularExcedente() retorna siempre un valor negativo o cero (demanda).

*/

class NodoConsumidor : public NodoRed {
public:
  enum class Perfil { Residencial, Comercial, Industrial };

private:
  Perfil PerfilConsumo;

public:
  double calcularExcedente(); // Implementarlo

  NodoConsumidor();
  ~NodoConsumidor();
};

/*
NodoProsumidor (hereda de NodoRed):
  - Atributos adicionales:
    double produccion,
    double consumo.

  - calcularExcedente() retorna produccion - consumo (positivo si hay
excedente).

*/

class NodoProsumidor : public NodoRed {
private:
  double produccion;
  double consumo;

public:
  double calcularExcedente(); // Implementarlo

  NodoProsumidor(/* args */);
  ~NodoProsumidor();
};

/*

NodoAlmacenamiento (hereda de NodoRed):
  - Atributo adicional:
  double cargaActual (kWh).

  - Sobrescribe métodos para absorber y liberar energía.
      TransaccionEnergia (entidad):
        - int idVendedor,
          idComprador;
          double kwh;
          double precio;
          std::chrono::system_clock::time_point timestamp;
          Orden (struct):
              int idOrden;
              bool esCompra; // true=compra, false=venta
              int idNodo;
              double precio;
              double kwh;
              uint64_t secuencia; // timestamp o contador FIFO

*/

class NodoAlmacenamiento : public NodoRed {
private:
  double cargaActual;

public:
  double calcularExcedente(); // Implementarlo - Hay que sobrescribir metodo
                              // para absorber y lierar energia
  NodoAlmacenamiento();
  ~NodoAlmacenamiento();
};

/*

GridManager: Contiene el libro de órdenes y ejecuta el matching.
  Método principal
    void procesarTick(const std::vector& ofertasCSV);
    CapaDatos: Se conecta a Oracle usando SOCI.

  Métodos:
    void persistirTransac­ciones(const std::vector& trans); etc.

*/

class GridManager {
private:
  // void procesarTick(const std::vector &ofertasCSV);

public:
  GridManager();

  // void persistirTransacciones(const std::vector &trasn);

  ~GridManager();
};

#endif // types_h
