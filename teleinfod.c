#include <sys/stat.h>
#include <stdio.h>   /* Standard input/output definitions */
#include <string.h>  /* String function definitions */
#include <unistd.h>  /* UNIX standard function definitions */
#include <fcntl.h>   /* File control definitions */
#include <errno.h>   /* Error number definitions */
#include <termios.h> /* POSIX terminal control definitions */
#include <pthread.h>
#include <stdlib.h>
#include <syslog.h>
#include <ctype.h>
#include <getopt.h>
#include <sqlite3.h>

// Define constante de paramétrage
#define VERSION "20160901"
#define BAUDRATE B1200 //soit 150 Octets par secondes
#define NB_VALEURS 12 // Nombre de valeurs à relever du compteur ErDF (12 pour un compteur monophasé).
#define NB_SECOND 300 // Nombre de secondes entre chaque écriture en base SQLite
#define BUFFER_MESURE 512 //Agrandir la taille des tableaux en fonction de l'intervalle d'écriture en base
#define SQLITE_TABLE "teleinfo"

/* Création de la table SQLite teleinfo :
CREATE TABLE "teleinfo" (
	`date`	TEXT NOT NULL UNIQUE,
	`HP`	INTEGER DEFAULT 0,
	`HC`	INTEGER DEFAULT 0,
	`PTEC`	TEXT,
	`ISOUSC`	INTEGER DEFAULT 30,
	`IINST`	INTEGER DEFAULT 0,
	`IMAX`	INTEGER DEFAULT 30,
	`PAPP`	INTEGER DEFAULT 0
)
*/

static char *SQLITE_DB = "/mnt/usbkey/domotique.sqlite";
static int  debug = 0;
static int	daemonize = 0;

/* Structure stockant les informations des threads teleinfo et timer. */
struct teleinfo_shared {
	char valeurs[NB_VALEURS][14];
	int  buf_isousc;
	int  buf_hchp;
	int  buf_hchc;
	char buf_ptec[4];
	int  buf_iinst[BUFFER_MESURE];
	int  buf_imax;
	int  buf_papp[BUFFER_MESURE];
	int	 count_buf;

	pthread_mutex_t mut;
};

struct teleinfo_data {
   int param;
   char const *sid;

   struct teleinfo_shared *ti_sh;
};

double getAverage(int arr[], int size) {
	double avg = 0, sum = 0;

	for (int i = 0; i < size; ++i) { if (arr[i] > 0) sum += arr[i]; }
	avg = sum / size;

	return avg;
}

/*------------------------------------------------------------------------------*/
/* Init port rs232																*/
/*------------------------------------------------------------------------------*/
int initserie(char * device_serial) { // Mode Non-Canonical Input Processing, Attend 1 caractère ou time-out(avec VMIN et VTIME).
	int				device = 0;
	struct termios	termiosteleinfo;

	if ((device = open(device_serial, O_RDONLY | O_NOCTTY)) == -1) { // Ouverture de la liaison serie
		syslog(LOG_ERR, "Erreur ouverture du port serie %s !", device_serial);
		return -1;
	}

	tcgetattr(device,  & termiosteleinfo); // Lecture des parametres courants.
	cfsetispeed( & termiosteleinfo, BAUDRATE); // Configure le débit en entrée/sortie.
	cfsetospeed( & termiosteleinfo, BAUDRATE);
	termiosteleinfo.c_cflag |= (CLOCAL | CREAD); // Active réception et mode local.

	/* 7E1 */
	termiosteleinfo.c_cflag |= PARENB; // Active 7 bits de donnees avec parite pair.
	termiosteleinfo.c_cflag &= ~PARODD;
	termiosteleinfo.c_cflag &= ~CSTOPB;
	termiosteleinfo.c_cflag &= ~CSIZE;
	termiosteleinfo.c_cflag |= CS7;
	termiosteleinfo.c_iflag |= (INPCK | ISTRIP); // Mode de control de parité.

	termiosteleinfo.c_cflag &= ~CRTSCTS; // Désactive control de flux matériel.
	termiosteleinfo.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // Mode non-canonique (mode raw) sans echo.
	termiosteleinfo.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL); // Désactive control de flux logiciel, conversion 0xOD en 0x0A.
	termiosteleinfo.c_oflag &= ~OPOST; // Pas de mode de sortie particulier (mode raw).
	termiosteleinfo.c_cc[VTIME] = 80; // time-out à ~8s.
	termiosteleinfo.c_cc[VMIN] = 0; // 1 car. attendu.
	tcflush(device, TCIFLUSH); // Efface les données reçues mais non lues.
	tcsetattr(device, TCSANOW,  & termiosteleinfo); // Sauvegarde des nouveaux parametres

	return device;
}

/*------------------------------------------------------------------------------*/
/* Test checksum d'un message (Return 1 si checkum ok)				*/
/*------------------------------------------------------------------------------*/
int checksum_ok(char * etiquette, char * valeur, char checksum) {
	unsigned char sum = 32; // Somme des codes ASCII du message + un espace

	for (unsigned int i = 0; i < strlen(etiquette); i++) sum = sum + etiquette[i];
	for (unsigned int i = 0; i < strlen(valeur); i++) sum = sum + valeur[i];
	sum = (sum & 63) + 32;
	if (sum == checksum) return 1; // Return 1 si checkum ok.

	return 0;
}

/*------------------------------------------------------------------------------*/
/* Ecrit les données teleinfo dans une base sqlite3      						*/
/*------------------------------------------------------------------------------*/
int writesqliteteleinfo(char data[]) {
	sqlite3 * db;
	char * err_msg = 0;
	char sql[1024] = "";

	int rc = sqlite3_open(SQLITE_DB,  & db);

	if (rc != SQLITE_OK) {
		if (debug) syslog(LOG_ERR, "Cannot open database: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		return 1;
	}

	sprintf(sql, "INSERT OR REPLACE INTO %s (date, ISOUSC, HP, HC, PTEC, IINST, IMAX, PAPP) VALUES (%s);", SQLITE_TABLE, data);
	rc = sqlite3_exec(db, sql, 0, 0,  & err_msg);

	if (rc != SQLITE_OK) {
		if (debug) syslog(LOG_ERR, "SQL error: %s\n", err_msg);
		sqlite3_free(err_msg);
		sqlite3_close(db);
		return 1;
	} else {
		if (debug) syslog(LOG_NOTICE, "Write SQL: %s\n", sql);
	}

	sqlite3_close(db);
	return 0;
}

// Timer pour vidage du buffer
void *timer_writesql(void *data) {
	struct teleinfo_data *p_data = data;
	time_t td;
	char timestamp[21];
	char datateleinfo[512];
	double avg_iinst = 0, avg_papp = 0;

	while (1) { //Le timer est une boucle infinie avec une pause de NB_SECOND entre chaque itération
		struct tm * dc;

		sleep(NB_SECOND);

		time(&td);
		dc = localtime(&td);
		strftime(timestamp, sizeof timestamp, "%Y-%m-%d %H:%M:%S", dc);

		if ((p_data->ti_sh->buf_hchp > 0) && (p_data->ti_sh->buf_hchc > 0)) { //Si des données d'heures creuses et pleines sont présentes
			pthread_mutex_lock (&p_data->ti_sh->mut); {
				avg_iinst = getAverage(p_data->ti_sh->buf_iinst, p_data->ti_sh->count_buf);
				avg_papp = getAverage(p_data->ti_sh->buf_papp, p_data->ti_sh->count_buf);
				sprintf(datateleinfo,
					"'%s','%d','%d','%d','%s','%10.2lf','%d','%10.2lf'", 
					timestamp, 
					p_data->ti_sh->buf_isousc, 
					p_data->ti_sh->buf_hchp, 
					p_data->ti_sh->buf_hchc, 
					p_data->ti_sh->buf_ptec, 
					avg_iinst, 
					p_data->ti_sh->buf_imax, 
					avg_papp);
				p_data->ti_sh->count_buf = 0;
			}
			pthread_mutex_unlock (&p_data->ti_sh->mut);
			writesqliteteleinfo(datateleinfo); // Ecriture des donnees dans la base SQLite.
		}
	}

	return NULL;
}

void *read_teleinfo(void *data) {
	struct teleinfo_data *p_data = data;
	int erreur_checksum = 0;
	int fdserial = p_data->param;

	while (1) {
		char   message[256];
		char   etiquettes[NB_VALEURS][12] = { "ADCO", "OPTARIF", "ISOUSC", "HCHP", "HCHC", "PTEC", "IINST", "IMAX", "PAPP", "HHPHC", "MOTDETAT", "ADPS" };
		char   car_prec;
		char   ch[2];
		char   checksum[32];

		tcflush(fdserial, TCIFLUSH); // Efface les données non lus en entrée.
		message[0] = '\0';

		pthread_mutex_lock (&p_data->ti_sh->mut);
		memset(p_data->ti_sh->valeurs, 0x00, sizeof(p_data->ti_sh->valeurs)); //Zeroing
		pthread_mutex_unlock (&p_data->ti_sh->mut);

		erreur_checksum = 0;

		// Lecture données téléinfo (0d 03 02 0a => Code fin et début trame)
		while (!(ch[0] == 0x02 && car_prec == 0x03)) { // Attend code fin suivi de début trame téléinfo .
			car_prec = ch[0];
			if (!read(fdserial, ch, 1) && debug) syslog(LOG_ERR, "Erreur pas de réception début données Téléinfo !\n");
		} 

		//Lit les données jusqu'au code de fin de trame
		while (ch[0] != 0x03) { // Attend code fin trame téléinfo.
			if (!read(fdserial, ch, 1) && debug) syslog(LOG_ERR, "Erreur pas de réception fin données Téléinfo !\n");
			ch[1] = '\0';
			strcat(message, ch); //place les données lues dans la variable message
		}

		for (int id = 0; id < NB_VALEURS; id++) { // Recherche valeurs des étiquettes de la liste.
			char * match;

			if ((match = strstr(message, etiquettes[id])) != NULL) {
				pthread_mutex_lock (&p_data->ti_sh->mut); {
					sscanf(match, "%s %s %s", etiquettes[id], p_data->ti_sh->valeurs[id], checksum);
					if (strlen(checksum) > 1) checksum[0] = ' '; // sscanf ne peux lire le checksum à 0x20 (espace), si longueur checksum > 1 donc c'est un espace.
					if (!checksum_ok(etiquettes[id], p_data->ti_sh->valeurs[id], checksum[0])) {
						erreur_checksum = 1;
						if (debug) syslog(LOG_ERR, "Donnees teleinfo [%s] corrompues !\n", etiquettes[id]);
					}
				}	
				pthread_mutex_unlock (&p_data->ti_sh->mut);
			}
		}

		if (!erreur_checksum) {
			if ((strlen(p_data->ti_sh->valeurs[3])) && (strlen(p_data->ti_sh->valeurs[4])) && (strlen(p_data->ti_sh->valeurs[8]))) { // Test si valeurs HCHP, HCHC, PAPP sont vides (possible aprés trame ADPS).
				pthread_mutex_lock (&p_data->ti_sh->mut); {
					p_data->ti_sh->valeurs[5][2] = '\0'; // Remplace chaine "HP.." ou "HC.." par "HP ou "HC".

					p_data->ti_sh->buf_isousc = atoi(p_data->ti_sh->valeurs[2]);
					p_data->ti_sh->buf_hchp = atoi(p_data->ti_sh->valeurs[3]);
					p_data->ti_sh->buf_hchc = atoi(p_data->ti_sh->valeurs[4]);

					if (strlen(p_data->ti_sh->valeurs[5]) > 1) strcpy(p_data->ti_sh->buf_ptec, p_data->ti_sh->valeurs[5]);

					p_data->ti_sh->buf_iinst[p_data->ti_sh->count_buf] = atoi(p_data->ti_sh->valeurs[6]);
					p_data->ti_sh->buf_imax = atoi(p_data->ti_sh->valeurs[7]); ;
					p_data->ti_sh->buf_papp[p_data->ti_sh->count_buf] = atoi(p_data->ti_sh->valeurs[8]);

					p_data->ti_sh->count_buf++;
				}	
				pthread_mutex_unlock (&p_data->ti_sh->mut);
				if ((strlen(p_data->ti_sh->valeurs[11])) && debug) syslog(LOG_INFO, "Dépassement d'intensité: ADPS='%s' !", p_data->ti_sh->valeurs[11]); // Test si etiquette dépassement intensité sur 1 phase (ADPS).
			} else {
				if (debug) syslog(LOG_ERR, "Donnees teleinfo vide: HCHP=%s, HCHC=%s, PAPP=%s !\n", p_data->ti_sh->valeurs[3], p_data->ti_sh->valeurs[4], p_data->ti_sh->valeurs[8]);
			}
		}
	}

	return NULL;
}

int main(int argc, char * argv[]) {
	char *			device_serial = "/dev/ttyAMA0";
	int 			c = 0;
	int  			fdserial = 0;
	const char * 	short_opt = "db:s:vh:";
	struct option 	long_opt[] = { {
		"daemon", 		  no_argument, NULL, 'd' }, {
		"db", 		required_argument, NULL, 'b' }, {
		"serial",	required_argument, NULL, 's' }, {
		"verbose", 		  no_argument, NULL, 'v' }, {
		"help", 		  no_argument, NULL, 'h' }, {
		NULL, 					    0, NULL,  0  }
	};

	while ((c = getopt_long(argc, argv, short_opt, long_opt, NULL)) != EOF) {
		switch (c) {
			case -1: /* no more arguments */
			case 0: /* long options toggles */
				break;

			case 'd':
				daemonize = 1;
				break;

			case 'b':
				SQLITE_DB = optarg;
				break;

			case 's':
				device_serial = optarg;
				break;

			case 'v':
				debug = 1;
				break;

			case 'h':
				printf("Usage: %s [OPTIONS]\n", argv[0]);
				printf("  -d, --daemon            run in daemon mode\n");
				printf("  -b, --db file           path to sqlite db (default: /mnt/usbkey/domotique.sqlite)\n");
				printf("  -s, --serial device     path to serial device (default: /dev/ttyAMA0)\n");
				printf("  -v, --verbose           enable verbose into syslog\n");
				printf("  -h, --help              print this help and exit\n");
				printf("\n");
				return (0);

			case ':':
			case '?':
				fprintf(stderr, "Try `%s --help' for more information.\n", argv[0]);
				return (-2);

			default:
				fprintf(stderr, "%s: invalid option -- %c\n", argv[0], c);
				fprintf(stderr, "Try `%s --help' for more information.\n", argv[0]);
				return (-2);
		};
	};

	if (daemonize == 1) {
		pid_t pid, sid; /* Our process ID and Session ID */

		pid = fork(); /* Fork off the parent process */
		if (pid < 0) { return EXIT_FAILURE; }
		if (pid > 0) { return EXIT_SUCCESS; } /* If we got a good PID, then we can exit the parent process. */

		umask(0); /* Change the file mode mask */

		sid = setsid(); /* Create a new SID for the child process */
		if (sid < 0) { return EXIT_FAILURE; }

		close(STDIN_FILENO); /* Close out the standard file descriptors */
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
	}

	openlog("teleinfod", LOG_NOWAIT | LOG_PID, LOG_USER); /* Open a connection to the syslog server */

	syslog(LOG_NOTICE, "Successfully started teleinfod version : %s (libsqlite3 %s)\n", VERSION, sqlite3_libversion()); /* Sends a message to the syslog daemon */
	if (daemonize == 1) syslog(LOG_NOTICE, "Running teleinfod in daemon mode\n");

	fdserial = initserie(device_serial);
	if (fdserial >= 0) {
		pthread_t thread_id_read_teleinfo;
		pthread_t thread_id_timer;

		struct teleinfo_shared sh = {
			.buf_isousc = 0,
			.buf_hchp = 0,
			.buf_hchc = 0,
			.buf_imax = 0,
			.count_buf = 0,
			.mut = PTHREAD_MUTEX_INITIALIZER,
		};

		struct teleinfo_data data_teleinfo = {
			.param = fdserial,
			.sid = "Teleinfo",
			.ti_sh = &sh,
		};

		struct teleinfo_data data_timer = {
			.param = NB_SECOND,
			.sid = "Timer",
			.ti_sh = &sh,
		};

		pthread_create (&thread_id_timer, NULL, timer_writesql, &data_timer);
		pthread_create (&thread_id_read_teleinfo, NULL, read_teleinfo, &data_teleinfo);

		pthread_join (thread_id_read_teleinfo, NULL);
		pthread_join (thread_id_timer, NULL);
	}

	syslog(LOG_INFO, "Exit teleinfod ...\n");
	close(fdserial);
	closelog();
	return EXIT_SUCCESS;
}
