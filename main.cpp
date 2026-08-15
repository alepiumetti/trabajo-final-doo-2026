#include <sqlite3.h>
#include <iostream>

int main() {
    sqlite3* db = nullptr;

    if (sqlite3_open("ejemplo.db", &db) != SQLITE_OK) {
        std::cerr << "No se pudo abrir la base de datos\n";
        return 1;
    }

    const char* sql = R"SQL(
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS NODOS (
  id_nodo INTEGER PRIMARY KEY,
  ubicacion TEXT NOT NULL,
  tipo TEXT NOT NULL CHECK (tipo IN ('Consumidor', 'Prosumidor', 'Bateria')),
  saldo_cuenta REAL DEFAULT 0 CHECK (saldo_cuenta >= 0),
  perfil_consumo TEXT
);

CREATE TABLE IF NOT EXISTS LECTURAS_HISTORICAS (
  id_lectura INTEGER PRIMARY KEY AUTOINCREMENT,
  id_nodo INTEGER NOT NULL,
  tick_hora TEXT NOT NULL,
  produccion_kwh REAL DEFAULT 0,
  consumo_kwh REAL DEFAULT 0,
  excedente_neto REAL,
  FOREIGN KEY (id_nodo) REFERENCES NODOS(id_nodo)
);

CREATE TABLE IF NOT EXISTS TRANSACCIONES (
  id_transaccion INTEGER PRIMARY KEY AUTOINCREMENT,
  id_vendedor INTEGER NOT NULL,
  id_comprador INTEGER NOT NULL,
  kwh REAL NOT NULL CHECK (kwh > 0),
  precio_unitario REAL NOT NULL CHECK (precio_unitario > 0),
  fecha_transaccion TEXT DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (id_vendedor) REFERENCES NODOS(id_nodo),
  FOREIGN KEY (id_comprador) REFERENCES NODOS(id_nodo)
);

CREATE TABLE IF NOT EXISTS CONFIG_TARIFAS (
  hora INTEGER PRIMARY KEY CHECK (hora BETWEEN 0 AND 23),
  precio_base_kwh REAL NOT NULL
);

CREATE TRIGGER IF NOT EXISTS trg_validar_saldo
BEFORE INSERT ON TRANSACCIONES
FOR EACH ROW
BEGIN
  SELECT CASE
    WHEN (
      SELECT saldo_cuenta
      FROM NODOS
      WHERE id_nodo = NEW.id_comprador
    ) < (NEW.kwh * NEW.precio_unitario)
    THEN RAISE(ABORT, 'Saldo insuficiente para realizar la compra')
  END;
END;
)SQL";

    char* errMsg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "Error SQL: " << errMsg << "\n";
        sqlite3_free(errMsg);
        sqlite3_close(db);
        return 1;
    }

    sqlite3_close(db);
    return 0;
}
