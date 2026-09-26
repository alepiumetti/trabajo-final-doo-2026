#ifndef config_h
#define config_h

#include <fstream>
#include <string>
#include <unordered_map>

struct Config {
  std::string dbPath = "./ejemplo.db";
  std::string datosDir = "./datos";
  std::string sqlPath = "./sql/crear_esquema.sql";
};

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
  }
  return cfg;
}

#endif // config_h