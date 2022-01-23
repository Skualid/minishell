#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include "parser.h"
#include <signal.h>
#include <fcntl.h>

#define CD "cd"
#define JOBS "jobs"
#define FG "fg"

//Variables globales//
tline *line;
pid_t processInBG[32];
char infoProcess[32][32];
int contProcess = 0;
int activaHandlerSiEsFG = 0;
//variable global que me sirve para pasarle al handler el proceso en foreground
int posProcessInBG;
//-------------//

//Funciones//
int getValueOfCommand(char * c);
void mycd();
void jobs();
void fgWithArg(int id);
void fg();
int inputRedirect();
int outputRedirect();
int errorRedirect();
void controlChandler(int sig);
void sigchildHandler(int sig);
void removeProcess(int posicion);
int getProcess(pid_t pid);
//----------//

int main() {
    char buf[1024];
    pid_t pid;

    // Ignoramos las señales. Si no se hace Control + C sobre un proceso en FG lo ignora.
    signal(SIGINT, controlChandler);
    signal(SIGQUIT, SIG_IGN);

    printf("\033[0;31mmsh> \033[0m");

    while (fgets(buf,1024,stdin)) {
        line  = tokenize(buf);

        if (line == NULL) {
            continue;
        }

        //Si solo hay un mandato, cd, jobs, fg también
        if (line->ncommands == 1) {
            //Miro si el mandato es el cd
            if (strcmp(line->commands[0].argv[0], CD) == 0) {
                mycd();
                //Miro si el mandato es jobs
            }else if (strcmp(line->commands[0].argv[0], JOBS) == 0) {
                jobs();
                //Miro si es el mandato fg
            }else if (strcmp(line->commands[0].argv[0], FG) == 0) {
                if (line->commands[0].argv[1] != NULL) { //fg con argumento
                    fgWithArg(atoi(line->commands[0].argv[1]));
                }else {//fg sin argumento
                    fg();
                }
            }else {// Si el mandato no es jobs ni cd, ni fg, ejecuto el que me pasen(si existe)
                pid = fork();
                int status;

                if (pid < 0) {
                    fprintf(stderr, "Fallo el fork()");
                    exit(-1);
                }else if (pid == 0) { //Hijo
                    if (line ->background) { //Si es BG me cambio el ppid si no no. Pues no me dejaría hacer control + C
                        setpgid(0, 0);
                    }
                    //Señales, acción por defecto.
                    //Cuando se ejecuta un mandato si queremos que el proceso responda ante estas señales
                    signal(SIGINT , SIG_DFL);
                    signal(SIGQUIT, SIG_DFL);

                    //Redirección de entrada y salida
                    if (line->redirect_input != NULL) {
                        if (inputRedirect() == -1) {
                            continue;
                        }
                        inputRedirect();
                    }
                    if (line->redirect_output != NULL) {
                        if (outputRedirect() == -1) {
                            continue;
                        }
                        outputRedirect();
                    }
                    if (line->redirect_error != NULL) {
                        if (errorRedirect() == -1) {
                            continue;
                        }
                        errorRedirect();
                    }

                    if (getValueOfCommand(line->commands[0].filename) == 0) { //Si el mandato existe
                        if (line -> background) {
                            //Si me mandan un comando en BG que recibe info por stdin, la cortamos. por ejemplo, sort &
                            int devNull = open("/dev/null", O_RDONLY);
                            dup2(devNull, STDIN_FILENO);
                        }

                        execvp(line->commands[0].filename, line->commands[0].argv); //En caso de éxito el proceso termina y devuelve un 0
                        //Si hay algún error el proceso continua
                        fprintf(stderr, "Se ha producido un error al ejecutar el comando: %s\n", strerror(errno));
                        exit(1);

                    }else { //Si el mandato no existe
                        fprintf(stderr, "%s : No se encuentra el mandato\n" , line->commands[0].argv[0]);
                    }
                }else { //Padre
                    //Si la variable background vale 1 quiere decir que nos han introducido un mandato con un & al final
                    //Comprobamos además que el comando sea válido y de ser así procedemos a pasar el proceso a BG
                    //Que básicamente es que el padre no debe esperar a que termine el hijo
                    if (line->background) {
                        if (getValueOfCommand(line->commands[0].filename) == 0) {
                            printf("Proceso con pid %d llevado a BG\n",pid);
                            strcpy(infoProcess[contProcess],buf); //Guardo la entrada estándar en una matriz de strings

                            processInBG[contProcess] = pid; //Guardo el pid del proceso que me han puesto en BG
                            contProcess++;
                        }
                    }else {
                        waitpid(pid, &status, 0); //Espero por el hijo
                        if (WIFEXITED(status) != 0) { //Me devuelve != 0 si el proceso terminó
                            if (WEXITSTATUS(status) != 0) { //Me dice cómo termino. Si es != 0 terminó con algún error
                                printf("Se produjo alguna anomalía con el comando\n");
                            }
                        }
                    }
                }
            }
        }else if (line->ncommands > 1) { //Si tenemos 2 o más mandatos
            pid = fork();
            int status;

            if (pid < 0) {
                fprintf(stderr, "Fallo el fork()");
                exit(-1);
            }else if (pid == 0) {
                //Guardamos el valor del pid del padre, pues determinará en que grupo van a estar
                //los siguientes comandos concatenados (nietos)
                pid_t ppidHijo = getpid(); //Recojo el pid del padre
                if (line ->background) { //Si es BG me cambio el ppid si no, no. Pues no me dejaría hacer control + C
                    setpgid(0, 0);
                }
                //Señales, acción por defecto.
                //Cuando se ejecuta un mandato si queremos que el proceso responda ante estas señales
                signal(SIGINT, SIG_DFL);
                signal(SIGQUIT, SIG_DFL);

                //Creo tantas tuberías como número de comandos -1 me pasen
                //Recordar que cada pipe tiene 2 descriptores( 0 -> read, 1 -> write )
                int pipes[line->ncommands - 1][2];

                for (int i = 0; i < line->ncommands - 1; i++) {
                    pipe(pipes[i]);
                }

                pid_t *nieto = (int*) malloc(line->ncommands * sizeof (int));

                for (int i = 0; i < line->ncommands; i++) {
                    nieto[i] = fork(); //Creo tantos nietos como comandos haya
                    int status_nietos;

                    if (nieto[i] < 0) {
                        fprintf(stderr, "Fallo el fork()");
                        exit(-1);
                    } else if (nieto[i] == 0) { //Nietos
                        if (getValueOfCommand(line->commands[i].filename) == 0) { //Si el mandato 'i' existe
                            if (i == 0) { //Primer nieto -> La entrada es estándar, pero su salida no
                                if (line ->background) { //Si es BG me cambio el ppid si no, no. Pues no me dejaría hacer control + C
                                    setpgid(0, ppidHijo);
                                }

                                //Redirección de entrada para el primer mandato del primer pipe
                                if (line->redirect_input != NULL) {
                                    if (inputRedirect() == -1) {
                                        continue;
                                    }
                                    inputRedirect();
                                }

                                close(pipes[i][0]); //Cierro la que no voy a utilizar, el primer comando no va a leer de la tubería sino de la entrada estándar
                                dup2(pipes[i][1], 1); //El primer comando escribe en la tubería, no en la salida estándar.

                                close(pipes[i][0]);//Cierro todos los pipes porque un proceso no manda un EOF si no están todos los descriptores (de los pipes) de escritura cerrados
                                //Por ejemplo en un sort el proceso anterior no mandaría el EOF, y el sort no acabaría nunca.

                                execvp(line->commands[i].filename, line->commands[i].argv); //En caso de éxito el proceso termina y devuelve un 0
                                //Si hay algún error el proceso continúa
                                fprintf(stderr, "Se ha producido un error al ejecutar el comando: %s\n", strerror(errno));
                                exit(1);
                            } else if (i == (line->ncommands - 1)) { // Último nieto -> la salida es estándar, la entrada no
                                if (line ->background) { //Si es BG me cambio el ppid si no, no. Pues no me dejaría hacer control + C
                                    setpgid(0, ppidHijo);
                                }
                                //Redirección de salida para el último mandato del último pipe
                                if (line->redirect_output != NULL) {
                                    if (outputRedirect() == -1) {
                                        continue;
                                    }
                                    outputRedirect();
                                }
                                if (line->redirect_error != NULL) {
                                    if (errorRedirect() == -1) {
                                        continue;
                                    }
                                    errorRedirect();
                                }

                                close(pipes[i - 1][1]); //Cierro la que no voy a utilizar. El último comando no escribe en el pipe sino en la salida estándar.
                                dup2(pipes[i - 1][0], 0); //El último comando lee de la tubería, no de la entrada estándar.

                                close(pipes[i - 1][0]);

                                execvp(line->commands[i].filename, line->commands[i].argv); //En caso de éxito el proceso termina y devuelve un 0
                                //Si hay algún error el proceso continúa
                                fprintf(stderr, "Se ha producido un error al ejecutar el comando: %s\n", strerror(errno));
                                exit(1);
                            } else { //Resto de nietos -> ni la entrada ni la salida es la estándar
                                if (line ->background) { //Si es BG me cambio el ppid si no, no. Pues no me dejaría hacer control + C
                                    setpgid(0, ppidHijo);
                                }

                                close(pipes[i - 1][1]);//Cierro la que no voy a utilizar. Un comando intermedio no escribe en el pipe anterior, sino en el siguiente
                                dup2(pipes[i - 1][0], 0);//Un comando intermedio lee del pipe anterior, no de la entrada estándar.

                                close(pipes[i][0]);//Cierro la que no voy a utilizar. Un comando intermedio no lee del pipe siguiente, sino del anterior.
                                dup2(pipes[i][1], 1);//Un comando intermedio escribe en el pipe siguiente, no en la salida estándar.

                                close(pipes[i - 1][0]);
                                close(pipes[i][1]);


                                execvp(line->commands[i].filename, line->commands[i].argv); //En caso de éxito el proceso termina y devuelve un 0
                                //Si hay algún error el proceso continúa
                                fprintf(stderr, "Se ha producido un error al ejecutar el comando: %s\n",strerror(errno));
                                exit(1);
                            }
                        } else {
                            fprintf(stderr, "%s : No se encuentra el mandato\n", line->commands[i].argv[0]);
                        }
                    }else {//Padre
                        if (i == 0) {
                            close(pipes[i][1]);
                        } else if (i==(line->ncommands-1)) {
                            close(pipes[i][0]);
                        } else {
                            close(pipes[i-1][0]);
                            close(pipes[i][1]);
                        }
                        waitpid(nieto[i], &status_nietos, 0); //Espero a que terminen todos los nietos
                    }
                }
            }else {//Abuelo (minishell)
                //Si la variable background vale 1 quiere decir que nos han introducido un mandato con un & al final
                //Procedemos a pasar el proceso a BG, que básicamente es que el abuelo no debe esperar a que termine el padre
                if (line->background) {
                    strcpy(infoProcess[contProcess], buf); //Guardo la entrada estándar en una matriz de strings

                    processInBG[contProcess] = pid; //Guardo el pid del proceso padre.
                    contProcess++;
                    printf("Proceso con pid %d llevado a BG\n", pid);
                } else {
                    waitpid(pid, &status, 0); //Espero por el hijo
                    if (WIFEXITED(status) != 0) { //Me devuelve != 0 si el proceso terminó
                        if (WEXITSTATUS(status) != 0) { //Me dice cómo termino. Si es != 0 terminó con algún error
                            printf("Se produjó alguna anomalia con el comando\n");
                        }
                    }
                }
            }
        }
        //Señal que se activa cuando un proceso hijo termina y está reprogramada
        signal(SIGCHLD, sigchildHandler);
        printf("\033[0;31mmsh> \033[0m");
    }
    return 0;
}


//Implementación de las funciones del programa//
int getValueOfCommand(char * c) { //Esta función nos devuelve un 0 si el comando es válido o -1 en caso contrario
    int output = 0;

    if (c == NULL) {
        output = -1;
    }
    return output;
}

void mycd() {
    char buf[1024];

    if (line->commands[0].argc > 2)   {
        printf("Demasiados argumentos\n");
    }else if (line->commands[0].argc == 2) { // el nombre del fichero que esta implícitamente siempre + el directorio
        if (chdir(line->commands[0].argv[1]) == 0)  { //Si chdir nos devuelve 0 el directorio es correcto
            chdir(line->commands[0].argv[1]);

            printf("El directorio actual es: %s\n", getcwd(buf,1024)); //Nos imprime el directorio actual
        }else { //Si chdir nos devuelve algo que no sea 0 no es un directorio válido
            printf("No es un directorio válido\n");
        }
    }else {  //Si no recibe ningún argumento me lleva a HOME
        chdir(getenv("HOME")); //getenv convierte las variables de entorno en string

        printf("El directorio actual es: %s\n", getcwd(buf,1024)); //Nos imprime el directorio actual
    }
}

void jobs() { //Tengo un array de pids en segundo plano que quiero mostrar al ejecutar jobs
    for (int k = 1; k <= contProcess; k++) {
        printf("[%d] Running  %s",k, infoProcess[k-1]);
    }
}

void fgWithArg(int id) { //Le paso el id que referencia al proceso que se quiere mandar a foreground
    if (id <= contProcess && id >= 0){ // si nos pasan un id válido
        id--;
        //Necesito un handler porque el Control + C lo recibe el padre, y le tiene que decir que lo haga el hijo
        //Aparte a cada hijo le asigno un grupo diferente, que va a ser igual a su pid. Porque si no lo cambio
        //todos los hijos van a tener el mismo group pid que el padre y cuando haga el control + C de un hijo
        //me los voy a cargar a todos.
        posProcessInBG = id;
        //Nos activa la variable para indicar que el Control + C va para un proceso en FG
        activaHandlerSiEsFG = 1;
        signal(SIGINT, controlChandler);
        waitpid(processInBG[id], NULL, 0);

        removeProcess(id); //Cuando el proceso en FG termina, lo elimino de la matriz de strings y del array de procesos en BG
    }else { //Si el id no es válido
        printf("Pasa un identificador válido\n");
    }
}

void fg() { //Paso a foreground el último mandato que me han pasado a background
    fgWithArg(contProcess);
}

int inputRedirect() {
    int fichero = open(line->redirect_input, O_RDONLY);
    if (fichero != -1) {//No ha habido fallo
        dup2(fichero,0);
        return 0;
    }else {//Ha habido fallo al abrir el fichero
        fprintf(stderr ,"Error. %s\n" ,strerror(errno));
        return -1;
    }
}

int outputRedirect() {
    //Por defecto, el creat es equivalente a open con CREAT, WRONLY, TRUNC
    int fichero = creat(line->redirect_output, 0664);
    if (fichero != -1) {//No ha habido fallo
        dup2(fichero,STDOUT_FILENO);
        return 0;
    }else {//Ha habido fallo al abrir el fichero
        fprintf(stderr ,"Error. %s\n" ,strerror(errno));
        return -1;
    }
}

int errorRedirect() {
    //Por defecto, el creat es equivalente a open con CREAT, WRONLY, TRUNC
    int fichero = creat(line->redirect_error, 0664);
    if (fichero != -1) {//No ha habido fallo
        dup2(fichero,2);
        return 0;
    }else {//Ha habido fallo al abrir el fichero
        fprintf(stderr ,"Error. %s\n" ,strerror(errno));
        return -1;
    }
}

void removeProcess(int posicion) {
    if (contProcess > 0) {
        //Ahora tengo que "quitar" del processInBG y del infoProcess el proceso que he pasado a foreground
        if (posicion == contProcess) { //si es el último
            contProcess--; //Dejo inaccesible al último elemento
        } else if (posicion < contProcess && posicion >= 0) { //cualquiera que no sea el último
            pid_t aux[contProcess];
            char aux1[contProcess][32];

            for (int k = 0; k < contProcess; k++) {//copio los arrays originales en auxiliares
                aux[k] = processInBG[k];
                strcpy(aux1[k], infoProcess[k]);
            }

            for (int k = posicion; k < contProcess - 1; k++) {//muevo los elementos y dejo en la última posición el proceso que quiero "eliminar"
                processInBG[k] = aux[k + 1];
                strcpy(infoProcess[k], aux1[k + 1]);
            }
            contProcess--; //Dejo inaccesible al último elemento
        }
    }
}

int getProcess(pid_t pid) {
    int contador = 0;

    while((processInBG[contador] != pid) && (contador < contProcess)) {
        contador++;
    }
    return contador;
}

void controlChandler(int sig) { // Si es un comando en FG que previamente a venido de BG, si no, ignora el Control + C
    if (activaHandlerSiEsFG) {
        //Mato al grupo entero
        killpg(processInBG[posProcessInBG], SIGINT);
        activaHandlerSiEsFG = 0;
    }
}

void sigchildHandler(int sig) {
    //Solo quiero comprobar si ha muerto algún proceso que tenga en BG, para eliminarlo de mi estructura
    for (int i = 0; i < contProcess; i++) {
        if (waitpid(processInBG[i], NULL, WNOHANG) > 0) {
            int posicion = getProcess(processInBG[i]);
            removeProcess(posicion);
        }
    }
}