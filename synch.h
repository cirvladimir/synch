/** This file will have syncronization abilities for multiple processes.
  * A process can issue commands, then other processes may have to respond
  * when that command is completed. (c++)
  */
#include <fcntl.h>
#include <errno.h>
#include <thread>
#include <vector>
#include <queue>
#include <map>
#include <mutex>
#include <condition_variable>

namespace Synch {
class Command {
public:
    int id, value;
    bool requireResponse;
};

class Synch {
private:
    struct ComS {
        int comId;
        int id, value;
        bool requireResponse;
        bool synch;
    };

    struct CliResp {
        int comId;
    };

    void removeClient(int i) {
        clients.erase(clients.begin() + i);
        for (auto it = comResps.cbegin(); it != comResps.cend();)
        {
            comResps[it]--;
            if (comResps[it] == 0)
            {
                comResps.erase(it++);
            }
            else
            {
                ++it;
            }
        }
    }

    int findComId() {
        if (comResps.isEmpty()) {
            return 32;
        }
        int id = (32 + 432) % 234521;
        while (comResps.find(id) != comResps.end()) {
            id = (id + 432) % 234521;
        }
        return id;
    }

    void run_server() {
        while (isRunning) {
            int rd, tries, totRd;
            int MAX_TRIES = 10;
            struct sockaddr_in serv_addr;
            int n;
            int sockfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd < 0) {
                printf("ERROR opening socket\n");
                continue;
            }
            bzero((char *) &serv_addr, sizeof(serv_addr));
            serv_addr.sin_family = AF_INET;
            serv_addr.sin_addr.s_addr = INADDR_ANY;
            serv_addr.sin_port = htons(port);
            int optval = 1;
            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval);
            if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
                printf("ERROR on binding\n");
                continue;
            }
            listen(sockfd,5);

            fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

            std::unique_lock<std::mutex> lck(mtx);
            while (isRunning) {
                // listen for new clients
                acceptClient();
                
                // listen for responses
                for (int i = clients.size() - 1; i >= 0; i--) {
                    struct CliResp resp;
                    rd = read(clients[i],(char*)&resp, sizeof(resp));
                    if (rd > 0) {
                        tries = 0;
                        totRd = td;
                        while ((totRd < sizeof(resp)) && (tries < MAX_TRIES)) {
                            rd = read(clients[i],(char*)&resp + totRd, sizeof(resp) - totRd);
                            if ((rd < 0) && (errno !=  EAGAIN && errno != EWOULDBLOCK)) {
                                printf("Error reading from client after not enough data.\n    %s\n Dropping...\n", strerror(errno));
                                break;
                            }
                            totRd += rd;
                        }
                        if (totRd < sizeof(resp)) {
                            printf("Client didn't send enough data.\n    %s\n Dropping...\n", strerror(errno));
                            removeClient(i);
                            continue;
                        }
                        if (comResps.find(resp.comId) != comResps.end()) {
                            comResps[resp.comId] = comResps[resp.comId] - 1;
                            if (comResps[resp.comId] <= 0) {
                                comResps.erase(resp.comId);
                            }
                        }
                    } else if ((rd < 0) && (errno !=  EAGAIN && errno != EWOULDBLOCK)) {
                        // Drop connection
                        printf("Error reading from client.\n    %s\n Dropping...\n", strerror(errno));
                        removeClient(i);
                    }
                }

                // send out commands
                if (!commands.isEmpty()) {
                    struct ComS c = commands.pop();
                    c.comId = findComId();
                    for (int i = clients.size() - 1; i >= 0; i--) {
                        int wr = write(clients[i], (char*)&c, sizeof(c));
                        if (wr != sizeof(c)) {
                            printf("Error sending command.\n    %s\n Dropping...\n", strerror(errno));
                            removeClient(i);
                        }
                    }
                    if (c.requireResponse) {
                        if (clients.size() > 0)
                            comResps[c.comId] = clients.size();
                    }
                }

                // end loop and wait
                if (comResps.size() == 0)
                    comRespCv.notifyAll();
                lck.unlock();
                sleep(0.01);
                lck.lock();
            }

            // cleanup
            close(sockfd);
        }
    }
    std::vector<int> clients;

    void acceptClient() {
        struct sockaddr_in cli_addr;
        socklen_t clilen = sizeof(cli_addr);
        int newsockfd = accept4(sockfd, (struct sockaddr *) &cli_addr, &clilen, SOCK_NONBLOCK);
        if (newsockfd > 0) {
            printf("Client accepted\n");
            clients.push_back(newsockfd);
        }
    }

    void run_client() {
        while (isRunning) {
            // Connection code:
            int sockfd, rd;
            struct sockaddr_in serv_addr;
            struct hostent *server;

            sockfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd < 0) {
                fprintf("ERROR opening socket\n");
                continue;
            }
            server = gethostbyname("127.0.0.1");
            if (server == NULL) {
                fprintf("ERROR, no such host\n");
                continue;
            }
            bzero((char *) &serv_addr, sizeof(serv_addr));
            serv_addr.sin_family = AF_INET;
            bcopy((char *)server->h_addr, 
                (char *)&serv_addr.sin_addr.s_addr,
                server->h_length);
            serv_addr.sin_port = htons(port);
            if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) {
                fprintf("ERROR connecting\n");
                continue;
            }
            
            std::unique_lock<std::mutex> lck(mtx);
            while (isRunning) {
                struct ComS inCom;
                rd = read(sockfd, (char*)&inCom, sizeof(inCom));
                if ((rd < 0) && (errno !=  EAGAIN && errno != EWOULDBLOCK)) {
                    // Drop connection
                    printf("Error reading from server.\n    %s\n Reconnecting...\n", strerror(errno));
                    break;
                } else if ((rd > 0) && (rd < sizeof(inCom))) {
                    printf("Wrong amount read\n    %s\n Reconnecting...\n", strerror(errno));
                    break;
                } else if (rd == sizeof(inCom)) {
                    commands.push(inCom);
                    if (inCom.requireResponse) {
                        comResps[inCom.comId] = inCom.id;
                    }
                    comRespCv.notifyAll();
                }

                lck.unlock();
                sleep(0.01);
                lck.lock();
            }

            // cleanup
            close(sockfd);
        }
    }

public:
    int port;
    std::thread * runThread;
    bool isRunning;
    std::queue<ComS> commands;
    std::map<int, int> comResps;
    std::mutex mtx;
    std::condition_variable comRespCv;

    Synch(int _port) {
        port = _port;
        runThread = NULL;
        isRunning = false;
    }

    void startServer() {
        if (isRunning)
            return;
        isRunning = true;
        runThread = new std::thread(run_server);
    }

    void stopServer() {
        if (!isRunning)
            return;
        std::unique_lock<std::mutex> lck(mtx);
        isRunning = false;
        lck.unlock();
        runThread.join();
        clients.clear();
    }

    void startClient() {
        if (isRunning)
            return;
        isRunning = true;
        runThread = new std::thread(run_client);
    }

    void stopClient() {
        if (!isRunning)
            return;
        std::unique_lock<std::mutex> lck(mtx);
        isRunning = false;
        lck.unlock();
        runThread.join();
        clients.clear();
    }

    void issueSynchCommand(Command com) {
        std::unique_lock<std::mutex> lck(mtx);
        ComS c;
        c.id = com.id;
        c.value = com.value;
        c.requireResponse = true;
        c.synch = true;
        commands.push(c);
        lck.unlock();
        waitAllResponses();
    }

    void issueAsynchCommand(Command com) {
        std::unique_lock<std::mutex> lck(mtx);
        ComS c;
        c.id = com.id;
        c.value = com.value;
        c.requireResponse = com.requireResponse;
        c.synch = false;
        commands.push(c);
    }

    bool allResponded() {
        std::unique_lock<std::mutex> lck(mtx);
        return comResps.size() == 0;
    }

    void waitAllResponses() {
        std::unique_lock<std::mutex> lck(mtx);
        while (comResps.size() > 0) {
            comRespCv.wait(lck);
        }
    }

    bool hasCommand() {
        std::unique_lock<std::mutex> lck(mtx);
        return commands.isEmpty();
    }

    Command getCommand() {
        std::unique_lock<std::mutex> lck(mtx);
        while (commands.isEmpty()) {
            comRespCv.wait(lck);
        }
        return comResps.pop();
    }

    void respond(int comId) {
        
    }
};
}
