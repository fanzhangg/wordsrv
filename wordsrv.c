#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>

#include "socket.h"
#include "gameplay.h"


#ifndef PORT
#define PORT y
#endif
#define MAX_QUEUE 5


void add_player(struct client **top, int fd, struct in_addr addr);
void remove_player(struct client **top, int fd);

/* These are some of the function prototypes that we used in our solution 
 * You are not required to write functions that match these prototypes, but
 * you may find the helpful when thinking about operations in your program.
 */
/* Send the message in outbuf to all clients */
int broadcast(struct game_state *game, char *outbuf);
int announce_turn(struct game_state *game);
int announce_winner(struct game_state *game, struct client *winner);
/* Move the has_next_turn pointer to the next active client */
void advance_turn(struct game_state *game);
int Read(int fd, void *buf, size_t nbyte);
int read_from_input(char *line, int fd);
int restart_game(struct game_state *game, int fd, char *dict);
int check_name(struct game_state *game, int fd, char *name);
int check_good_guess(struct game_state *game, int guess);
void disconnect_from_game(struct game_state *game, int fd);


/* The set of socket descriptors for select to monitor.
 * This is a global variable because we need to remove socket descriptors
 * from allset when a write to a socket fails.
 */
fd_set allset;


/* Add a client to the head of the linked list
 */
void add_player(struct client **top, int fd, struct in_addr addr) {
    struct client *p = malloc(sizeof(struct client));

    if (!p) {
        perror("malloc");
        exit(1);
    }

    printf("Adding client %s\n", inet_ntoa(addr));

    p->fd = fd;
    p->ipaddr = addr;
    p->name[0] = '\0';
    p->in_ptr = p->inbuf;
    p->inbuf[0] = '\0';
    p->next = *top;
    *top = p;
}

/* Removes client from the linked list and closes its socket.
 * Also removes socket descriptor from allset 
 */
void remove_player(struct client **top, int fd) {
    struct client **p;

    for (p = top; *p && (*p)->fd != fd; p = &(*p)->next);
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
        FD_CLR((*p)->fd, &allset);
        close((*p)->fd);
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
                fd);
    }
}

/* Send the message in outbuf to all clients 
 * Return -1 if detected disconnection. 
 */
int broadcast(struct game_state *game, char *outbuf) {
    struct client *cur_client = game->head; // the client pointer for traversal

    while (cur_client) {
        /* Send message to all clients. */
        if (write(cur_client->fd, outbuf, strlen(outbuf)) == -1) {
            return -1;
        }
        cur_client = cur_client->next;
    }
    return 0;
}

/* Announce the next turn of game. 
 * Return -1 if detected disconnection. 
 */
int announce_turn(struct game_state *game) {
    struct client *cur_client = game->head; // the client pointer for traversal
    char msg[MAX_MSG];                      // the messege container

    /* Display turn message in server. */
    sprintf(msg, "It's %s's turn.\n", game->has_next_turn->name);
    printf("%s", msg);

    while (cur_client) {
        /* Send guess message to the next player. */
        if (cur_client->fd == game->has_next_turn->fd) {
            if (write(cur_client->fd, GUESS_MSG, strlen(GUESS_MSG)) == -1) {
                return -1;
            }
        }
        /* Send turn message to other clients. */
        else {
            if (write(cur_client->fd, msg, strlen(msg)) == -1) {
                return -1;
            }
        }
        cur_client = cur_client->next;
    }
    return 0;
}

/* Announce winner aa the the winner of game. 
 * Return -1 if detected disconnection. 
 */
int announce_winner(struct game_state *game, struct client *winner) {
    struct client *cur_client = game->head; // the client pointer for traversal
    char msg[MAX_MSG];                      // the messege container

    /* Send word message to all clients. */
    sprintf(msg, "The word was %s. \n", game->word);
    if (broadcast(game, msg) == -1) {
        return -1;
    }
    /* Display winner message in server. */
    sprintf(msg, "Game over! %s won!\n\n", winner->name);
    printf("%s", msg);

    while (cur_client) {
        /* Send winner message to the winner. */
        if (cur_client->fd == winner->fd) {
            if (write(cur_client->fd, WIN_MSG, strlen(WIN_MSG)) == -1) {
                return -1;
            }
        }
        /* Send winner message to other clients. */
        else {
            if (write(cur_client->fd, msg, strlen(msg)) == -1) {
                return -1;
            }
        }
        cur_client = cur_client->next;
    }
    return 0;
}

/* Move the has_next_turn pointer to the next active client */
void advance_turn(struct game_state *game) {
    /* Set has_next_turn NULL if the last client leaves. */
    if (game->head == NULL) {
        game->has_next_turn = NULL;
    }
    /* Next turn points to the head in two cases below: 
     * has_next_turn has not been set or
     * has_next_turn is the last player of the active list.
     */
    else if (game->has_next_turn == NULL || game->has_next_turn->next == NULL) {
        game->has_next_turn = game->head;
    }
    /* Turn to next. */
    else {
        game->has_next_turn = game->has_next_turn->next;
    }
}

/* wrapper function for read */
int Read(int fd, void *buf, size_t nbyte) {
    int bytes = read(fd, buf, nbyte);

    if (bytes == -1) {
        perror("read");
        exit(1);
    }
    return bytes;
}

/* Read the input into line, dealing with network newline. 
 * Return the total number of characters read. 
 */
int read_from_input(char *line, int fd) {
    line[0] = '\0'; // Initialize line to an empty string.

    /* First read from input. */
    int num_chars1 = Read(fd, line, MAX_BUF);
    line[num_chars1] = '\0';
    printf("[%d] Read %d bytes\n", fd, num_chars1);

    /* It may take more than one read to get all of the data that was written. */
    while (num_chars1 > 0 && strstr(line, "\r\n") == NULL) {
        int num_chars2 = Read(fd, &line[num_chars1], MAX_BUF - num_chars1);
        printf("[%d] Read %d bytes\n", fd, num_chars2);
        num_chars1 += num_chars2;
        line[num_chars1] = '\0';
    }

    /* Strip trailing network newline and display nonempty input. */
    line[num_chars1 - 2] = '\0';
    if (strlen(line) > 0) {
        printf("[%d] Found newline %s\n", fd, line);
    }
    return num_chars1;
}

/* Restart a game with a new word. */
int restart_game(struct game_state *game, int fd, char *dict) {
    /* Send new game message to all. */
    printf("New game\n");
    if (broadcast(game, "Let's start a new game\n") == -1) {
        return -1;
    }
    init_game(game, dict); // Initialize a new game. 
    return 0;
}

/* Check if the input name is valid. */
int check_name(struct game_state *game, int fd, char *name) {
    struct client *cur_client = game->head; // the client pointer for traversal

    /* Check empty name: */
    if (strlen(name) == 0) {
        /* Send empty name message to the current client. */
        if (write(fd, EMPTY_NAME_MSG, strlen(EMPTY_NAME_MSG)) == -1) {
            return -1;
        }
        return 0;
    }
    /* Check duplicate name: */
    while (cur_client) {
        if (strcmp(cur_client->name, name) == 0) {
            /* Send duplicate name message to the current client. */
            if (write(fd, DUPLICATE_NAME_MSG, strlen(DUPLICATE_NAME_MSG)) == -1) {
                return -1;
            }
            return 0;
        }
        cur_client = cur_client->next;
    }
    return 1;
}

/* Check if the guess letter has not already been guessed and is in the word. */
int check_good_guess(struct game_state *game, int guess) {
    int good_guess = 0; // the indicator of good guess

    /* Check if guess is in letter_guessed. */
    if (game->letters_guessed[guess - 'a'] == 0) {
        game->letters_guessed[guess - 'a'] = 1; // Change the indicator of this guess letter in letter_guessed. 
        /* Check if this letter is in the word. */
        for (int i = 0; i < strlen(game->word); i++) {
            if (game->word[i] == guess) {
                game->guess[i] = guess;
                good_guess = 1;
            }
        }
    }
    return good_guess;
}

/* Disconnect client with fd from game until there is no more disconnection detected by write function. */
void disconnect_from_game(struct game_state *game, int fd) {
    struct client *p;  // the client pointer for traversal
    char msg[MAX_MSG]; // the messege container

    for (p = game->head; p && p->fd != fd; p = p->next);  // Find the client p with fd.
    printf("Disconnect from %s\n", inet_ntoa(p->ipaddr)); // Display disconnect message in server.
    
    /*  Save important data temporarily. */
    sprintf(msg, "Goodbye %s\n", p->name);
    int next_fd = game->has_next_turn->fd;

    /* This is for preventing has_next_turn become unaccessable after remove_player. */
    if (next_fd == fd) {
        game->has_next_turn = NULL;
    }

    remove_player(&(game->head), fd); // Remove player with fd from game.
    /* Advance turn if the disconnet client is the next player. */
    if (next_fd == fd) {
        advance_turn(game);
    }
    /* Announce turn and send goodbye message to all clients unless there is no active client. */
    if (game->head != NULL) {
        if (broadcast(game, msg) == -1) {
            disconnect_from_game(game, fd); // Recursion for more disconnection in broadcast.
        }
        if (announce_turn(game) == -1) {
            disconnect_from_game(game, fd); // Recursion for more disconnection in broadcast.
        }
    }
    return;
}


int main(int argc, char **argv) {
    int clientfd, maxfd, nready;
    struct client *p;
    struct sockaddr_in q;
    fd_set rset;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <dictionary filename>\n", argv[0]);
        exit(1);
    }

    // Create and initialize the game state
    struct game_state game;

    srandom((unsigned int) time(NULL));
    // Set up the file pointer outside of init_game because we want to 
    // just rewind the file when we need to pick a new word
    game.dict.fp = NULL;
    game.dict.size = get_file_length(argv[1]);

    init_game(&game, argv[1]);

    // head and has_next_turn also don't change when a subsequent game is
    // started so we initialize them here.
    game.head = NULL;
    game.has_next_turn = NULL;

    /* A list of client who have not yet entered their name.  This list is
     * kept separate from the list of active players in the game, because
     * until the new playrs have entered a name, they should not have a turn
     * or receive broadcast messages.  In other words, they can't play until
     * they have a name.
     */
    struct client *new_players = NULL;

    struct sockaddr_in *server = init_server_addr(PORT);
    int listenfd = set_up_server_socket(server, MAX_QUEUE);

    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)) {
            printf("A new client is connecting\n");
            clientfd = accept_connection(listenfd);

            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("Connection from %s\n", inet_ntoa(q.sin_addr));
            add_player(&new_players, clientfd, q.sin_addr);
            char *greeting = WELCOME_MSG;
            if (write(clientfd, greeting, strlen(greeting)) == -1) {
                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(q.sin_addr));
                remove_player(&new_players, clientfd);
            };
        }

        /* Check which other socket descriptors have something ready to read.
         * The reason we iterate over the rset descriptors at the top level and
         * search through the two lists of clients each time is that it is
         * possible that a client will be removed in the middle of one of the
         * operations. This is also why we call break after handling the input.
         * If a client has been removed the loop variables may not longer be 
         * valid.
         */
        int cur_fd;
        for (cur_fd = 0; cur_fd <= maxfd; cur_fd++) {
            if (FD_ISSET(cur_fd, &rset)) {
                // Check if this socket descriptor is an active player

                for (p = game.head; p != NULL; p = p->next) {
                    if (cur_fd == p->fd) {
                        char msg[MAX_MSG];  // the messege container

                        /* Check whether the client disconnect when input a name, */
                        if (read_from_input(p->inbuf, cur_fd) == 0) {
                            disconnect_from_game(&game, cur_fd);
                            break;
                        }

                        /* For the next player, */
                        if (game.has_next_turn->fd == p->fd) {
                            int guess = p->inbuf[0]; // the guessed letter

                            /* Check the validity of guess. */
                            if (strlen(p->inbuf) != 1 || guess < 'a' || guess > 'z') {
                                if (write(cur_fd, INVALID_GUESS_MSG, strlen(INVALID_GUESS_MSG)) == -1) {
                                    disconnect_from_game(&game, cur_fd);
                                    break;
                                }
                            } else {
                                /* Display guesses message to all clients. */
                                sprintf(msg, "%s guesses: %c\n", game.has_next_turn->name, guess);
                                if (broadcast(&game, msg) == -1) {
                                    disconnect_from_game(&game, cur_fd);
                                    break;
                                }

                                int good_guess = check_good_guess(&game, guess); // the indicator of good guess

                                /* If it is not a good guess, */
                                if (!good_guess) {
                                    /* Display bad guess message to all. */
                                    sprintf(msg, "%c is not in the word\n", guess);
                                    if (write(cur_fd, msg, strlen(msg)) == -1) {
                                        disconnect_from_game(&game, cur_fd);
                                        break;
                                    }
                                    printf("Letter %s", msg);
                                    /* Do guesses_left deccrement and turn to next player. */
                                    game.guesses_left--;
                                    advance_turn(&game);
                                    /* If there is no guesses remaining, */
                                    if (game.guesses_left == 0) {
                                        /* Display lose message to all. */
                                        printf("Evaluating for game_over\n");
                                        sprintf(msg, "No guesses left. Game over.\nThe word was %s. \n\n", game.word);
                                        if (broadcast(&game, msg) == -1) {
                                            disconnect_from_game(&game, cur_fd);
                                            break;
                                        }
                                        /* Restart a game. */
                                        if (restart_game(&game, cur_fd, argv[1]) == -1) {
                                            disconnect_from_game(&game, cur_fd);
                                            break;
                                        }
                                    }
                                }
                                    /* If the word has been reached, */
                                else if (strcmp(game.guess, game.word) == 0) {
                                    /* Announce the winner. */
                                    if (announce_winner(&game, game.has_next_turn) == -1) {
                                        disconnect_from_game(&game, cur_fd);
                                        break;
                                    }
                                    /* Restart a game. */
                                    if (restart_game(&game, cur_fd, argv[1]) == -1) {
                                        disconnect_from_game(&game, cur_fd);
                                        break;
                                    }
                                }

                                /* Display status and turn message to all clients. */
                                status_message(msg, &game);
                                if (broadcast(&game, msg) == -1) {
                                    disconnect_from_game(&game, cur_fd);
                                    break;
                                }
                                if (announce_turn(&game) == -1) {
                                    disconnect_from_game(&game, cur_fd);
                                    break;
                                }
                            }
                        }
                            /* For other players, */
                        else {
                            if (strlen(p->inbuf) > 0) {
                                /* Display not turn message to mistyping players. */
                                if (write(cur_fd, NOT_TURN_MSG, strlen(NOT_TURN_MSG)) == -1) {
                                    disconnect_from_game(&game, cur_fd);
                                    break;
                                }
                            }
                        }

                        break;
                    }
                }

                // Check if any new players are entering their names
                for (p = new_players; p != NULL; p = p->next) {
                    if (cur_fd == p->fd) {
                        char msg[MAX_MSG]; // the messege container
                        int valid_name;    // the indicator of valid name

                        /* Check whether the client disconnect when input a name, */
                        if (read_from_input(p->name, cur_fd) == 0) {
                            printf("Disconnect from %s\n", inet_ntoa(q.sin_addr));
                            remove_player(&new_players, cur_fd);
                            break;
                        }

                        /* Check whether it is a valid name or disconnect here. */
                        if ((valid_name = check_name(&game, cur_fd, p->name)) == -1) {
                            printf("Disconnect from %s\n", inet_ntoa(q.sin_addr));
                            remove_player(&new_players, cur_fd);
                            break;
                        }

                        /* If name input by the client is valid, deal with it. 
                         * Otherwise, wait for the next iteration. 
                         */
                        if (valid_name) {
                            struct client *pre_client = new_players; // the client pointer for traversal

                            /* Remove p from new_players. */
                            if (pre_client->fd == cur_fd) {
                                new_players = pre_client->next;
                            } else {
                                while (pre_client && pre_client->next->fd != cur_fd) {
                                    pre_client = pre_client->next;
                                }
                                pre_client->next = p->next;
                            }

                            /* Add p to active linked list. */
                            p->next = game.head;
                            game.head = p;

                            /* Display join message to all. */
                            sprintf(msg, "%s has just joined.\n", game.head->name);
                            printf("%s", msg);
                            if (broadcast(&game, msg) == -1) {
                                printf("Disconnect from %s\n", inet_ntoa(q.sin_addr));
                                remove_player(&new_players, cur_fd);
                                break;
                            }

                            /* Display status message to the new active player. */
                            status_message(msg, &game);
                            if (write(cur_fd, msg, strlen(msg)) == -1) {
                                printf("Disconnect from %s\n", inet_ntoa(q.sin_addr));
                                remove_player(&new_players, cur_fd);
                                break;
                            }

                            /* For fist active player, set him as the next turn. */
                            if (game.has_next_turn == NULL) {
                                advance_turn(&game);
                            }
                            /* Announce turn whenever a player join the game. */
                            if (announce_turn(&game) == -1) {
                                printf("Disconnect from %s\n", inet_ntoa(q.sin_addr));
                                remove_player(&new_players, cur_fd);
                                break;
                            }
                        }

                        break;
                    }
                }
            }
        }
    }
    return 0;
}



