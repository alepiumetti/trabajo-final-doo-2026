#ifndef config_h
#define config_h

#include <cstdio>
#include <fstream>
#include <string>

struct Config {
  std::string dbPath = "./ejemplo.db";
  std::string datosDir = "./datos";
  std::string sqlPath = "./sql/crear_esquema.sql";
  // Fecha base del día simulado (AAAA-MM-DD). Se combina con la hora
  // del tick para armar el timestamp de LECTURAS_HISTORICAS.
  std::string fechaSimulada = "2026-06-01";
};

// Una fecha tiene que verse AAAA-MM-DD. Si no, se avisa una vez y se usa
// la de respaldo: un valor mal escrito en config.ini no debe tumbar la
// simulación entera (el timestamp es la columna que el PDF pide como DATE).
inline bool fechaValida(const std::string &f) {
  if (f.size() != 10 || f[4] != '-' || f[7] != '-')
    return false;
  for (size_t i = 0; i < f.size(); ++i) {
    if (i == 4 || i == 7)
      continue;
    if (f[i] < '0' || f[i] > '9')
      return false;
  }
  return true;
}

// Timestamp simulado de un tick: "<fecha_simulada> <HH>:00:00".
inline std::string timestampTick(const Config &cfg, int hora) {
  const std::string &f = fechaValida(cfg.fechaSimulada) ? cfg.fechaSimulada
                                                         : std::string("2026-06-01");
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%s %02d:00:00", f.c_str(), hora);
  return std::string(buffer);
}

inline Config cargarConfig(const std::string &ruta = "./src/util/config.ini") {
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