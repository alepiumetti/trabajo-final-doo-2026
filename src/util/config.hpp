#ifndef config_h
#define config_h

#include <cstdio>
#include <fstream>
#include <string>

#include "../constantes.hpp"

struct Config {
  std::string dbPath = DB_PATH_DEFAULT;
  std::string datosDir = DATOS_DIR_DEFAULT;
  std::string sqlPath = SQL_PATH_DEFAULT;
  // Fecha base del día simulado (AAAA-MM-DD). Se combina con la hora
  // del tick para armar el timestamp de LECTURAS_HISTORICAS.
  std::string fechaSimulada = FECHA_SIMULADA_DEFAULT;
};

// Timestamp simulado de un tick: "<fecha_simulada> <HH>:00:00".
inline std::string timestampTick(const Config &cfg, int hora) {
  char buffer[BUFFER_TICK_TS];
  std::snprintf(buffer, sizeof(buffer), "%s %02d:00:00",
                cfg.fechaSimulada.c_str(), hora);
  return std::string(buffer);
}

inline Config cargarConfig(const std::string &ruta = CONFIG_INI_PATH) {
  Config cfg;
  std::ifstream archivo(ruta);
  if (!archivo.is_open()) {
    return cfg;
  }

  std::string linea;
  while (getline(archivo, linea)) {
    if (linea.empty() || linea[0] == '#') continue;

    auto eq = linea.find('=');
    if (eq == std::string::npos) continue;

    std::string clave = linea.substr(0, eq);
    std::string valor = linea.substr(eq + 1);

    auto trim = [](std::string s) {
      auto inicio = s.find_first_not_of(" \t\r");
      if (inicio == std::string::npos) return std::string();
      auto fin = s.find_last_not_of(" \t\r");
      return s.substr(inicio, fin - inicio + 1);
    };

    clave = trim(clave);
    valor = trim(valor);

    if (clave == "db_path") cfg.dbPath = valor;
    else if (clave == "datos_dir") cfg.datosDir = valor;
    else if (clave == "sql_path") cfg.sqlPath = valor;
    else if (clave == "fecha_simulada") cfg.fechaSimulada = valor;
  }
  return cfg;
}

#endif // config_h