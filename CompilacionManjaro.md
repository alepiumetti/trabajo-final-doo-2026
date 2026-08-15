# Compilación de un programa C++ con Oracle Instant Client y SOCI

## 1. Verificar rutas

Asegurate de que existan estas rutas en tu sistema:

- `/opt/oracle/instantclient_23_26/sdk/include`
- `/opt/oracle/instantclient_23_26`
- `/usr/local/lib`

## 2. Compilar

Reemplazá `nuevo_main.cpp` por el archivo que quieras compilar:

```sh
g++ -std=c++17 TU_ARCHIVO.cpp -o TU_EJECUTABLE \
  -I/opt/oracle/instantclient_23_26/sdk/include \
  -L/opt/oracle/instantclient_23_26 \
  -L/usr/local/lib \
  -lsoci_oracle -lsoci_core -lclntsh -locci -lnnz \
  -Wl,--allow-shlib-undefined
```

Ejemplo:

```sh
g++ -std=c++17 nuevo_main.cpp -o mi_programa \
  -I/opt/oracle/instantclient_23_26/sdk/include \
  -L/opt/oracle/instantclient_23_26 \
  -L/usr/local/lib \
  -lsoci_oracle -lsoci_core -lclntsh -locci -lnnz \
  -Wl,--allow-shlib-undefined
```

3. Configurar librerías en ejecución
   Antes de ejecutar, exportá:

`export LD_LIBRARY_PATH=/opt/oracle/instantclient_23_26:/usr/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}`

4. Ejecutar

`./TU_EJECUTABLE`

5. Si querés cambiar el archivo a compilar
   Solo reemplazá:

- TU_ARCHIVO.cpp
- TU_EJECUTABLE

por los nombres que necesites.
