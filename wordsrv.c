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
#include <signal.h>

#include "socket.h"
#include "gameplay.h"


#ifndef PORT
    #define PORT 59042
#endif
#define MAX_QUEUE 5
#define BUF_SIZE 128

/* These are the given helper function */
void add_player(struct client **top, int fd, struct in_addr addr);
void remove_player(struct client **top, int fd);

/* Send the message in outbuf to all clients except special_player who is the current player. */
void broadcast(struct game_state game, char *outbuf, struct client *special_player);
/* This function is to loop to get a complete message from client. */
int read_partial_input_from_client(struct client *p, char *result);
/* Check if name already existed in game. */
int check_dup_name(struct game_state game, char *name);
/* Announce message msg to client, if that client is disconnected, remove him from list. */
void send_msg_to_client(struct client *p, char *msg, struct client **list);
/* Move the has_next_turn pointer to the next active client */
void advance_turn(struct game_state *game);
/* Announce the current player to guess and tell others whose turn it is. */
void announce_guess_and_turn(struct game_state game);
/* After guess letter c, update game->word. */
void generate_guess(struct game_state *game, char c);
/* Removes client from the linked list new_players without closing its socket. */
void remove_from_newplayers(struct client **new_players, int fd);
/* Check if letter is in the word word to be guessed. */
int check_exist(char letter, char *word);
/* A commonly used announce, including the guess status and announce_guess_and_turn. */
void one_turn(struct game_state game);
/* Start a new game. */
void new_game(struct game_state *game, char **argv);


/* The set of socket descriptors for select to monitor.
 * This is a global variable because we need to remove socket descriptors
 * from allset when a write to a socket fails.
 */
fd_set allset;

/* Add a client to the head of the linked list */
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

    for (p = top; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
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


int main(int argc, char **argv) {
    int clientfd, maxfd, nready;
    struct client *p;
    struct sockaddr_in q;
    fd_set rset;

    if(argc != 2){
        fprintf(stderr,"Usage: %s <dictionary filename>\n", argv[0]);
        exit(1);
    }
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if(sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    // Create and initialize the game state
    struct game_state game;

    srandom((unsigned int)time(NULL));
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

        if (FD_ISSET(listenfd, &rset)){
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
                remove_player(&(game.head), p->fd);
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
        for(cur_fd = 0; cur_fd <= maxfd; cur_fd++) {
            if(FD_ISSET(cur_fd, &rset)) {
                // Check if this socket descriptor is an active player
                if (cur_fd == listenfd) {
                    continue;
                }
                
                for(p = game.head; p != NULL; p = p->next){
                    if(cur_fd == p->fd) {
                        //TODO - handle input from an active client
                        //A player send messages not during his turn
                        if (cur_fd != game.has_next_turn->fd) {

                            char garbage[MAX_BUF];
                            int res = read_partial_input_from_client(p, garbage);

                            if (res == 1) {// garbage is completed
                                send_msg_to_client(p, "It is not your turn.\r\n", &game.head);
                                printf("Player %s tried to guess out of turn\n", p->name);
                            } 

                            else if (res == -1) {// This client is gone.
                                char goodbye[MAX_MSG];
                                sprintf(goodbye, "Goodbye %s\r\n", p->name);
                                remove_player(&game.head, p->fd);

                                // The last player disconnected.
                                if (game.head == NULL) {
                                    break;
                                }

                                // announce all clients who disconnected.
                                broadcast(game, goodbye, NULL);
                                // start a next turn.
                                announce_guess_and_turn(game);
                                break;
                            }

                        } 

                        else {//Player sends messages during his turn
                            char guess[MAX_BUF];
                            int res = read_partial_input_from_client(p, guess);
                            switch (res) {

                                case -1:
                                {// This client gone during his turn.
                                    char goodbye[MAX_MSG];
                                    sprintf(goodbye, "Goodbye %s\r\n", p->name);
                                    advance_turn(&game);
                                    remove_player(&game.head, p->fd);

                                    if (game.head == NULL) {
                                        break;
                                    }
                                    // announce all clients who disconnected.
                                    broadcast(game, goodbye, NULL);
                                    // start a next turn.
                                    announce_guess_and_turn(game);
                                    break;
                                }

                                case 0:
                                {// Player sent empty guess letter.
                                    send_msg_to_client(p, "Invalid guess. Your guess?\r\n", &game.head);
                                    break;
                                }

                                case 1:
                                {
                                    int input_length = strlen(guess);
                                    // Player input more than one letter.
                                    if (input_length != 1 || guess[0] < 'a' || guess[0] > 'z') {
                                        send_msg_to_client(p, "Invalid guess. Your guess?\r\n", &game.head);
                                        break;
                                    } else {// Player input a valid letter.
                                        int letter_pos = guess[0] - 'a';
                                        if(game.letters_guessed[letter_pos] == 1){// letter already been guessed
                                            send_msg_to_client(p, "Already guessed. Your guess again?\r\n", &game.head);
                                            break;
                                        }
                                        game.letters_guessed[letter_pos] = 1;

                                        // the guessed letter not in game.word
                                        if(check_exist(guess[0], game.word) == -1) {
                                            char wrong_guess[MAX_MSG];
                                            sprintf(wrong_guess, "%c is not in the word\r\n", guess[0]);
                                            // notify server
                                            printf("Letter %c is not in the word\n", guess[0]);
                                            send_msg_to_client(p, wrong_guess, &game.head);
                                            advance_turn(&game);
                                        } else {// do nothing
                                        }
                                        
                                        game.guesses_left--;
                                        char who_guess_what[MAX_BUF];
                                        sprintf(who_guess_what, "%s guesses: %c\r\n", p->name, guess[0]);
                                        broadcast(game, who_guess_what, NULL);
                                        generate_guess(&game, guess[0]);
                                        one_turn(game);

                                        // game ends when a player guesses the last hidden letter.
                                        if(strcmp(game.word, game.guess) == 0){

                                            send_msg_to_client(p, "Game over! You win!\r\n\r\n", &game.head);
                                            char who_won[MAX_BUF];
                                            sprintf(who_won, "Game over! %s won!\r\n\r\n", p->name);
                                            // notify server
                                            printf("Game over! %s won!\n", p->name);
                                            broadcast(game, who_won, p);

                                            // new game message
                                            new_game(&game, argv);
                                        }

                                        // game ends when the players have zero guesses remaining.
                                        else if(game.guesses_left == 0){

                                            char no_left[MAX_BUF];
                                            sprintf(no_left, "No guesses left. Game over.\r\n\r\n");
                                            printf("No guesses left. Game over.\n");
                                            broadcast(game, no_left, NULL);

                                            // new game message
                                            // new game message
                                            new_game(&game, argv);
                                        }

                                        // game does not end.
                                        else{
                                            announce_guess_and_turn(game);
                                        }

                                    }
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }
        
                // Check if any new players are entering their names
                for(p = new_players; p != NULL; p = p->next) {

                    if(cur_fd == p->fd) {

                        // TODO - handle input from an new client who has

                        char name[MAX_NAME];
                        int res = read_partial_input_from_client(p, name);
                        switch (res) {

                            case -1:
                            {// client gone before entering name.
                                printf("client fd=%d left game without entering a name\n", p->fd);
                                remove_player(&new_players, p->fd);
                                break;
                            }

                            case 0:
                            {// client input an empty string as name.
                                char *greeting = WELCOME_MSG;
                                if (write(cur_fd, greeting, strlen(greeting)) == -1) {
                                    fprintf(stderr, "Write to client %s failed\n", inet_ntoa(p->ipaddr));
                                    remove_player(&new_players, p->fd);
                                };
                                break;
                            }

                            case 1:
                            {// client input a valid string as name
                                //input name has already exist in game
                                if (check_dup_name(game, name)) {
                                    p->in_ptr = p->inbuf;
                                    char *greeting = WELCOME_MSG;

                                    if (write(cur_fd, greeting, strlen(greeting)) == -1) {
                                        fprintf(stderr, "Write to client %s failed\n", inet_ntoa(p->ipaddr));
                                        remove_player(&new_players, p->fd);
                                    };

                                   continue;
                                }

                                strncpy(p->name, name, MAX_NAME);
                                remove_from_newplayers(&new_players, p->fd);

                                if (game.has_next_turn == NULL && game.head == NULL) {
                                    game.has_next_turn = p;
                                }
                                else if (game.has_next_turn == NULL && game.head != NULL) {
                                    fprintf(stderr, "something wrong\n");
                                }

                                p->next = game.head;
                                game.head = p;

                                // notify server
                                printf("%s has just joined\n", p->name);

                                // notify clients
                                char new_player[MAX_MSG];
                                sprintf(new_player, "%s has just joined\r\n", p->name);
                                broadcast(game, new_player, NULL);
                                one_turn(game);
                                announce_guess_and_turn(game);
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


/* These are self writing helper functions. */

/* A commonly used announce, including the guess status and announce_guess_and_turn. */
void one_turn(struct game_state game){
    char word_guess[MAX_MSG];
    status_message(word_guess, &game);
    broadcast(game, word_guess, NULL);
}

/* Start a new game. */
void new_game(struct game_state *game, char **argv){
    init_game(game, argv[1]);
    broadcast(*game, "Let's start a new game\r\n", NULL);
    printf("New game.\n");
    one_turn(*game);
    announce_guess_and_turn(*game);
}

/* Removes client from the linked list new_players without closing its socket. */
void remove_from_newplayers(struct client **new_players, int fd){
    struct client **p;
    for (p = new_players; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
    if (*p) {
        struct client *t = (*p)->next;
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d from new_players, but I don't know about it\n", fd);
    } 
}

/* Check if letter is in the word word to be guessed. */
int check_exist(char letter, char *word){
    for(int i = 0; i < strlen(word); i++){
        if(letter == word[i]){
            return i;
        }
    }
    return -1;
}

/* After guess letter c, update game->word. */
void generate_guess(struct game_state *game, char c){
    for(int i = 0; i < strlen(game->word); i++){
        if(game->word[i] == c){
            game->guess[i] = c;
        }
    }
}

/* This function is to loop to get a complete message from client. */
int read_partial_input_from_client(struct client *p, char *result) {
    /*
        1. If this client is gone, return -1
        2. Empty string, return 0
        3. String completed read, return 1
        4. Else, return 2
    */

    int clientfd = p->fd;
    int read_num = read(clientfd, p->in_ptr, MAX_BUF);
    
    // This client is gone.
    if (read_num == 0) {
        return -1;
    }

    // Empty string.
    if (p->inbuf[0] == '\r' && p->inbuf[1] == '\n'){
        p->in_ptr = p->inbuf;
        return 0;
    }

    // String completed read.
    else if (p->in_ptr[read_num-2] == '\r' && p->in_ptr[read_num-1] == '\n') {
        p->in_ptr[read_num-2] = '\0';
        strncpy(result, p->inbuf, MAX_NAME);
        p->in_ptr = p->inbuf;
        return 1;
    } 

    // String not completed read, keep reading.
    else {
        p->in_ptr += read_num;
        return 2;
    }
    return -2;
}

/* Send the message in outbuf to all clients except special_player who is the current player. */
void broadcast(struct game_state game, char *outbuf, struct client *special_player) {
    struct client *temp = game.head;
    while (temp != NULL) {
        if (special_player != NULL && temp->fd == special_player->fd) {
            temp = temp->next;
            continue;
        } 
        else {
            if (dprintf(temp->fd, "%s", outbuf) < 0) {
                fprintf(stderr, "Write to client %s failed\r\n", inet_ntoa(temp->ipaddr));
            }
        }
        temp = temp->next;
    }
}

/* Announce message msg to client, if that client is disconnected, remove him from list. */
void send_msg_to_client(struct client *player, char *msg, struct client **list) {
    if (player == NULL) {
        return;
    }
    if (dprintf(player->fd, "%s", msg) < 0) {
        fprintf(stderr, "Write to client %s failed\n", inet_ntoa(player->ipaddr));
        remove_player(list, player->fd);
    }
}

/* Check if name already existed in game. */
int check_dup_name(struct game_state game, char *name) {
    struct client *tmp = game.head;
    while (tmp != NULL) {
        if (strcmp(tmp->name, name) == 0) {
            return 1;
        }
        tmp = tmp->next;
    }
    return 0;
}

/* Move the has_next_turn pointer to the next active client */
void advance_turn(struct game_state *game) {
    struct client *next = game->has_next_turn->next;
    if (next == NULL) {
        game->has_next_turn = game->head;
    } else {
        game->has_next_turn = next;
    }
}

/* Announce the current player to guess and tell others whose turn it is. */
void announce_guess_and_turn(struct game_state game){
    send_msg_to_client(game.has_next_turn, "Your guess?\r\n", &game.head);
    char turn_msg[MAX_MSG];
    sprintf(turn_msg, "It's %s's turn.\r\n", game.has_next_turn->name);
    broadcast(game, turn_msg, game.has_next_turn);
    // notify server
    printf("It's %s's turn.\n", game.has_next_turn->name);
}
