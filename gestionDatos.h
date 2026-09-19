#ifndef gestionDatos_h
#define gestionDatos_h

class gestionDatos {
public:
  gestionDatos();
  ~gestionDatos();

private:
public:
  void crearTablas(sqlite3 *db);
};

#endif // gestionDatos_h
