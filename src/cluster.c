/* Prez Cluster implementation.
 * Copyright(C) 2014 Sureshkumar Nedunchezhian. All rights reserved.
 *
 */

/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "prez.h"
#include "cluster.h"
#include "endianconv.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>

/* A global reference to myself is handy to make code more clear.
 * Myself always points to server.cluster->myself, that is, the clusterNode
 * that represents this node. */
clusterNode *myself = NULL;
int node_synced = 1;

clusterNode *createClusterNode(char *nodename, int flags);
int clusterAddNode(clusterNode *node);
clusterNode *clusterLookupNode(char *name);
void clusterDelNode(clusterNode *delnode);

void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterReadHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterDoBeforeSleep(int flags);

/* -----------------------------------------------------------------------------
 * Initialization
 * -------------------------------------------------------------------------- */

/* Load the cluster config from 'filename'.
 *
 * If the file does not exist or is zero-length (this may happen because
 * when we lock the nodes.conf file, we create a zero-length one for the
 * sake of locking if it does not already exist), PREZ_ERR is returned.
 * If the configuration was loaded from the file, PREZ_OK is returned. */
int clusterLoadConfig(char *filename) {
    FILE *fp = fopen(filename,"r");
    char *line;
    int maxline;

    if (filename == NULL) {
        prezLog(PREZ_WARNING,
                "Error: no cluster config file specified. "
                "Need atleast 3 nodes for prez to work");
        exit(1);
    }
    if (fp == NULL) {
        if (errno == ENOENT) {
            return PREZ_ERR;
        } else {
            prezLog(PREZ_WARNING,
                "Loading the cluster node config from %s: %s",
                filename, strerror(errno));
            exit(1);
        }
    }

    /* Parse the file. Note that single liens of the cluster config file can
     * be really long as they include all the hash slots of the node.
     * This means in the worst possible case, half of the prez slots will be
     * present in a single line, possibly in importing or migrating state, so
     * together with the node ID of the sender/receiver.
     *
     * To simplify we allocate 1024+PREZ_CLUSTER_SLOTS*128 bytes per line. */
    maxline = 1024;
    line = zmalloc(maxline);
    while(fgets(line,maxline,fp) != NULL) {
        int argc;
        sds *argv;
        clusterNode *n;
        char *p;

        /* Skip blank lines, they can be created either by users manually
         * editing nodes.conf or by the config writing process if stopped
         * before the truncate() call. */
        if (line[0] == '\n') continue;

        /* Split the line into arguments for processing. */
        argv = sdssplitargs(line,&argc);
        if (argv == NULL) goto fmterr;
       /* Create this node if it does not exist */
        n = clusterLookupNode(argv[0]);
        if (!n) {
            n = createClusterNode(argv[0],0);
            clusterAddNode(n);
        }

        if (!strcasecmp(argv[0], server.name)) {
            n->flags |= PREZ_NODE_MYSELF;
            myself = server.cluster->myself = n;
        }

        /* Address and port */
        if ((p = strchr(argv[1],':')) == NULL) goto fmterr;
        *p = '\0';
        memcpy(n->ip,argv[1],strlen(argv[1])+1);
        n->port = atoi(p+2);

        sdsfreesplitres(argv,argc);
    }
    zfree(line);
    fclose(fp);

    /* Config sanity check */
    prezAssert(server.cluster->myself != NULL);
    prezLog(PREZ_NOTICE,"Node configuration loaded, I'm %.40s", myself->name);
    return PREZ_OK;

fmterr:
    prezLog(PREZ_WARNING,
        "Unrecoverable error: corrupted cluster config file.");
    fclose(fp);
    exit(1);
}

void initClusterConfig(void) {
    server.cluster = zmalloc(sizeof(clusterState));
    server.cluster->myself = NULL;
    server.cluster->size = 1;
    server.cluster->stats_bus_messages_sent = 0;
    server.cluster->stats_bus_messages_received = 0;
    server.cluster->election_timeout = PREZ_CLUSTER_ELECTION_TIMEOUT;
    server.cluster->heartbeat_interval = PREZ_CLUSTER_HEARTBEAT_INTERVAL;

    return;
}

void clusterInit(void) {

    server.cluster->nodes = dictCreate(&clusterNodesDictType,NULL); // 保存所有节点
    server.cluster->state = PREZ_FOLLOWER; // 开始只能是跟随者
    server.cluster->leader = sdsempty();
    server.cluster->voted_for = sdsempty();
    server.cluster->synced_nodes = dictCreate(&clusterNodesDictType,NULL);
    server.cluster->proc_clients = dictCreate(&clusterProcClientsDictType,NULL);

    server.cluster->current_term = 0;
    server.cluster->commit_index = 0;
    server.cluster->last_applied = 0;
    server.cluster->votes_granted = 0;

    server.cluster->log_filename = zstrdup(PREZ_DEFAULT_LOG_FILENAME);
    server.cluster->log_entries = listCreate();
    server.cluster->log_max_entries_per_request = PREZ_LOG_MAX_ENTRIES_PER_REQUEST;

    server.cluster->last_activity_time = mstime();

    /* Load or create a new nodes configuration. */
    if (clusterLoadConfig(server.cluster_configfile) == PREZ_ERR) {
        /* No configuration found. We will just use the random name provided
         * by the createClusterNode() function. */
        myself = server.cluster->myself =
            createClusterNode(NULL,PREZ_NODE_MYSELF);//PREZ_NODE_MYSELF|PREZ_NODE_MASTER);
        prezLog(PREZ_NOTICE,"No cluster configuration found, I'm %.40s",
                myself->name);
        clusterAddNode(myself);
    }

    /* We need a listening TCP port for our cluster messaging needs. */
    server.cfd_count = 0;

    if (listenToPort(server.cport, server.cfd,&server.cfd_count)
        == PREZ_ERR)
    {
        exit(1);
    } else {
        int j;
        prezLog(PREZ_DEBUG, "Cluster:%s started listening on:%d",
                server.name, server.cport);

        for (j = 0; j < server.cfd_count; j++) {
            if (aeCreateFileEvent(server.el, server.cfd[j], AE_READABLE,
                        clusterAcceptHandler, NULL) == AE_ERR)
                prezPanic("Unrecoverable error creating prez Cluster "
                        "file event.");
        }
    }
    /* Set myself->port to my listening port, we'll just need to discover
     * the IP address via MEET messages. */
    myself->port = server.port;

    /* Load Log File */
    if (loadLogFile() == PREZ_OK)
        prezLog(PREZ_NOTICE, "Prez log loaded from file");
    else if (errno != ENOENT) {
        prezLog(PREZ_WARNING,"Fatal error loading the prez log: %s. Exiting.",strerror(errno));
    }
    server.cluster->current_term = logCurrentTerm();
}

/* -----------------------------------------------------------------------------
 * Internal Utilities
 * -------------------------------------------------------------------------- */

static int compareIndices(const void *a, const void *b) {
        return (*(long long*)a)-(*(long long*)b);
}

static void reverseIndices(long long *indices, int size) {
    int i,j=size-1;
    long long temp;
    for(i=0;i<size/2;i++,j--) {
        temp = indices[j];
        indices[j] = indices[i];
        indices[i] = temp;
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER communication link
 * -------------------------------------------------------------------------- */

clusterLink *createClusterLink(clusterNode *node) {
    clusterLink *link = zmalloc(sizeof(*link));
    link->ctime = mstime();
    link->sndbuf = sdsempty();
    link->rcvbuf = sdsempty();
    link->node = node;
    link->fd = -1;
    return link;
}

/* Free a cluster link, but does not free the associated node of course.
 * This function will just make sure that the original node associated
 * with this link will have the 'link' field set to NULL. */
void freeClusterLink(clusterLink *link) {
    if (link->fd != -1) {
        aeDeleteFileEvent(server.el, link->fd, AE_WRITABLE);
        aeDeleteFileEvent(server.el, link->fd, AE_READABLE);
    }
    sdsfree(link->sndbuf);
    sdsfree(link->rcvbuf);
    if (link->node)
        link->node->link = NULL;
    close(link->fd);
    zfree(link);
}

#define MAX_CLUSTER_ACCEPTS_PER_CALL 1000
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    int max = MAX_CLUSTER_ACCEPTS_PER_CALL;
    char cip[PREZ_IP_STR_LEN];
    clusterLink *link;
    PREZ_NOTUSED(el);
    PREZ_NOTUSED(mask);
    PREZ_NOTUSED(privdata);
    while(max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                prezLog(PREZ_VERBOSE,
                    "Accepting cluster node: %s", server.neterr);
            return;
        }
        anetNonBlock(NULL,cfd);
        anetEnableTcpNoDelay(NULL,cfd);

        /* Use non-blocking I/O for cluster messages. */
        prezLog(PREZ_VERBOSE,"Accepted cluster node %s:%d", cip, cport);
        /* Create a link object we use to handle the connection.
         * It gets passed to the readable handler when data is available.
         * Initiallly the link->node pointer is set to NULL as we don't know
         * which node is, but the right node is references once we know the
         * node identity. */
        link = createClusterLink(NULL);
        link->fd = cfd;
        aeCreateFileEvent(server.el,cfd,AE_READABLE,clusterReadHandler,link);
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER node API
 * -------------------------------------------------------------------------- */

/* Create a new cluster node, with the specified flags.
 * If "nodename" is NULL this is considered a first handshake and a random
 * node name is assigned to this node (it will be fixed later when we'll
 * receive the first pong).
 *
 * The node is created and returned to the user, but it is not automatically
 * added to the nodes hash table. */
clusterNode *createClusterNode(char *nodename, int flags) {
    clusterNode *node = zmalloc(sizeof(*node));

    if (nodename)
        memcpy(node->name, nodename, PREZ_CLUSTER_NAMELEN);
    else
        getRandomHexChars(node->name, PREZ_CLUSTER_NAMELEN);
    node->ctime = mstime();
    node->link = NULL;
    memset(node->ip,0,sizeof(node->ip));
    node->port = 0;
    return node;
}

void freeClusterNode(clusterNode *n) {
    sds nodename;

    nodename = sdsnewlen(n->name, PREZ_CLUSTER_NAMELEN);
    prezAssert(dictDelete(server.cluster->nodes,nodename) == DICT_OK);
    sdsfree(nodename);
    if (n->link) freeClusterLink(n->link);
    zfree(n);
}

/* Add a node to the nodes hash table */
int clusterAddNode(clusterNode *node) {
    int retval;

    retval = dictAdd(server.cluster->nodes,
            sdsnewlen(node->name,PREZ_CLUSTER_NAMELEN), node);
    return (retval == DICT_OK) ? PREZ_OK : PREZ_ERR;
}

/* Remove a node from the cluster:
 * 1) Mark all the nodes handled by it as unassigned.
 * 2) Remove all the failure reports sent by this node.
 * 3) Free the node, that will in turn remove it from the hash table
 *    and from the list of slaves of its master, if it is a slave node.
 */
void clusterDelNode(clusterNode *delnode) {
    /* 1) Free the node, unlinking it from the cluster. */
    freeClusterNode(delnode);
}

/* Node lookup by name */
clusterNode *clusterLookupNode(char *name) {
    sds s = sdsnewlen(name, PREZ_CLUSTER_NAMELEN);
    dictEntry *de;

    de = dictFind(server.cluster->nodes,s);
    sdsfree(s);
    if (de == NULL) return NULL;
    return dictGetVal(de);
}

/* This is only used after the handshake. When we connect a given IP/PORT
 * as a result of CLUSTER MEET we don't have the node name yet, so we
 * pick a random one, and will fix it when we receive the PONG request using
 * this function. */
void clusterRenameNode(clusterNode *node, char *newname) {
    int retval;
    sds s = sdsnewlen(node->name, PREZ_CLUSTER_NAMELEN);

    prezLog(PREZ_DEBUG,"Renaming node %.40s into %.40s",
        node->name, newname);
    retval = dictDelete(server.cluster->nodes, s);
    sdsfree(s);
    prezAssert(retval == DICT_OK);
    memcpy(node->name, newname, PREZ_CLUSTER_NAMELEN);
    clusterAddNode(node);
}

void clusterProcessCommand(prezClient *c) {
    logEntry entry;
    long long commit_index;
    int j;
    sds cmdrepr = sdsnew("");

    memset(&entry,0,sizeof(logEntry));

    entry.index = logCurrentIndex()+1;
    entry.term = server.cluster->current_term;
    memcpy(entry.commandName,c->cmd->name,strlen(c->cmd->name)+1);

    for (j=0;j<c->argc;j++) {
        cmdrepr = sdscatprintf(cmdrepr,(char*)c->argv[j]->ptr,
                sdslen(c->argv[j]->ptr));
        if (j != c->argc-1)
            cmdrepr = sdscatlen(cmdrepr," ",1);
    }
    sdscatlen(cmdrepr,"\n",1);
    memcpy(entry.command,cmdrepr,sdslen(cmdrepr));
    sdsfree(cmdrepr);

    logWriteEntry(entry);
    logSync();

    if(dictSize(server.cluster->nodes) == 1) {
        commit_index = logCurrentIndex();
        logCommitIndex(commit_index);
        prezLog(PREZ_DEBUG,"clusterProcessCommand: cmtidx: %lld",
                commit_index);
    } else {
        dictAdd(server.cluster->proc_clients,
                sdsfromlonglong(entry.index),c);
    }
}

int clusterProcessPacket(clusterLink *link) {
    clusterMsg *hdr = (clusterMsg*) link->rcvbuf;
    uint32_t totlen = ntohl(hdr->totlen);
    uint16_t type = ntohs(hdr->type);

    server.cluster->stats_bus_messages_received++;
    /* Perform sanity checks */
    if (totlen < 16) return 1; /* At least signature, version, totlen, count. */
    if (ntohs(hdr->ver) != 0) return 1; /* Can't handle versions other than 0.*/
    if (totlen > sdslen(link->rcvbuf)) return 1;

    if (type == CLUSTERMSG_TYPE_VOTEREQUEST) { // 处理投票请求
        uint32_t explen;
        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        explen += sizeof(clusterMsgDataRequestVote);
        if (totlen != explen) return 1;

        prezLog(PREZ_DEBUG,"RV Recv Req: %s, term: %lld, "
                "logidx: %lld, logterm: %lld",
                hdr->data.requestvote.vote.candidateid,
                hdr->data.requestvote.vote.term,
                hdr->data.requestvote.vote.last_log_index,
                hdr->data.requestvote.vote.last_log_term);

        clusterProcessRequestVote(link, hdr->data.requestvote.vote);

    } else if (type == CLUSTERMSG_TYPE_APPENDENTRIES) { // 添加日志请求
        uint32_t explen;
        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        explen += (sizeof(clusterMsgDataAppendEntries) -
                (sizeof(logEntry)*PREZ_LOG_MAX_ENTRIES_PER_REQUEST) +
                (ntohs(hdr->data.appendentries.entries.log_entries_count)) *
                sizeof(logEntry));
        prezLog(PREZ_DEBUG,"AE Recv Req: log_count:%d, sizeof: %u",
                ntohs(hdr->data.appendentries.entries.log_entries_count),
                explen);
        if (totlen != explen) return 1;

        prezLog(PREZ_DEBUG,"AE Recv Req: %s, term: %lld, "
                "logidx: %lld, leadercmt: %lld",
                hdr->data.appendentries.entries.leaderid,
                hdr->data.appendentries.entries.term,
                hdr->data.appendentries.entries.prev_log_index,
                hdr->data.appendentries.entries.leader_commit_index);

        clusterProcessAppendEntries(link, hdr->data.appendentries.entries);

    } else if (type == CLUSTERMSG_TYPE_VOTEREQUEST_RESP) { // 回复添加投票结果请求
        uint32_t explen;
        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        explen += sizeof(clusterMsgDataResponseVote);
        if (totlen != explen) return 1;

        prezLog(PREZ_DEBUG,"RV Recv Req: %s, "
                "currterm: %lld, term: %lld, granted: %d",
                hdr->sender,
                server.cluster->current_term,
                hdr->data.responsevote.vote.term,
                hdr->data.responsevote.vote.vote_granted);
        clusterProcessResponseVote(link, hdr->data.responsevote.vote);
        
    } else if (type == CLUSTERMSG_TYPE_APPENDENTRIES_RESP) { // 回复添加日志请求
        uint32_t explen;
        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        explen += sizeof(clusterMsgDataResponseAppendEntries);
        if (totlen != explen) return 1;

        prezLog(PREZ_DEBUG,"AE Recv Rep: %s, "
                "term: %lld, idx: %lld, cmtidx: %lld, ok: %d",
                link->node->name,
                hdr->data.responseappendentries.entries.term,
                hdr->data.responseappendentries.entries.index,
                hdr->data.responseappendentries.entries.commit_index,
                hdr->data.responseappendentries.entries.ok);

        clusterProcessResponseAppendEntries(link,
                hdr->data.responseappendentries.entries);
    }

    return 1;
}

/* This function is called when we detect the link with this node is lost.
   We set the node as no longer connected. The Cluster Cron will detect
   this connection and will try to get it connected again.

   Instead if the node is a temporary node used to accept a query, we
   completely free the node on error. */
void handleLinkIOError(clusterLink *link) {
    freeClusterLink(link);
}

/* Send data. This is handled using a trivial send buffer that gets
 * consumed by write(). We don't try to optimize this for speed too much
 * as this is a very low traffic channel. */
void clusterWriteHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    clusterLink *link = (clusterLink*) privdata;
    ssize_t nwritten;
    PREZ_NOTUSED(el);
    PREZ_NOTUSED(mask);

    nwritten = write(fd, link->sndbuf, sdslen(link->sndbuf));
    if (nwritten <= 0) {
        prezLog(PREZ_WARNING,"I/O error writing to node link: %s",
            strerror(errno));
        handleLinkIOError(link);
        return;
    }
    sdsrange(link->sndbuf,nwritten,-1);
    if (sdslen(link->sndbuf) == 0)
        aeDeleteFileEvent(server.el, link->fd, AE_WRITABLE);
}

/* Read data. Try to read the first field of the header first to check the
 * full length of the packet. When a whole packet is in memory this function
 * will call the function to process the packet. And so forth. */
void clusterReadHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    char buf[sizeof(clusterMsg)];
    ssize_t nread;
    clusterMsg *hdr;
    clusterLink *link = (clusterLink*) privdata;
    int readlen, rcvbuflen;
    PREZ_NOTUSED(el);
    PREZ_NOTUSED(mask);

    while(1) { /* Read as long as there is data to read. */
        rcvbuflen = sdslen(link->rcvbuf);
        if (rcvbuflen < 8) {
            /* First, obtain the first 8 bytes to get the full message
             * length. */
            readlen = 8 - rcvbuflen;
        } else {
            /* Finally read the full message. */
            hdr = (clusterMsg*) link->rcvbuf;
            if (rcvbuflen == 8) {
                /* Perform some sanity check on the message signature
                 * and length. */
                if (memcmp(hdr->sig,"RCmb",4) != 0 ||
                    ntohl(hdr->totlen) < CLUSTERMSG_MIN_LEN)
                {
                    prezLog(PREZ_WARNING,
                        "Bad message length or signature received "
                        "from Cluster bus.");
                    handleLinkIOError(link);
                    return;
                }
            }
            readlen = ntohl(hdr->totlen) - rcvbuflen;
            if (readlen > sizeof(buf)) readlen = sizeof(buf);
        }

        nread = read(fd,buf,readlen);
        if (nread == -1 && errno == EAGAIN) return; /* No more data ready. */

        if (nread <= 0) {
            /* I/O error... */
            prezLog(PREZ_DEBUG,"I/O error reading from node link: %s",
                (nread == 0) ? "connection closed" : strerror(errno));
            handleLinkIOError(link);
            return;
        } else {
            /* Read data and recast the pointer to the new buffer. */
            link->rcvbuf = sdscatlen(link->rcvbuf,buf,nread);
            hdr = (clusterMsg*) link->rcvbuf;
            rcvbuflen += nread;
        }

        /* Total length obtained? Process this packet. */
        if (rcvbuflen >= 8 && rcvbuflen == ntohl(hdr->totlen)) {
            if (clusterProcessPacket(link)) {
                sdsfree(link->rcvbuf);
                link->rcvbuf = sdsempty();
            } else {
                return; /* Link no longer valid. */
            }
        }
    }
}

/* Put stuff into the send buffer.
 *
 * It is guaranteed that this function will never have as a side effect
 * the link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with the same link later. */
void clusterSendMessage(clusterLink *link, unsigned char *msg, size_t msglen) {
    if (sdslen(link->sndbuf) == 0 && msglen != 0)
        aeCreateFileEvent(server.el,link->fd,AE_WRITABLE,
                    clusterWriteHandler,link);

    link->sndbuf = sdscatlen(link->sndbuf, msg, msglen);
    server.cluster->stats_bus_messages_sent++;
}

/* Send a message to all the nodes that are part of the cluster having
 * a connected link.
 *
 * It is guaranteed that this function will never have as a side effect
 * some node->link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with node links later. */
void clusterBroadcastMessage(void *buf, size_t len) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (!node->link) continue;
        if (node->flags & (PREZ_NODE_MYSELF))
            continue;
        clusterSendMessage(node->link,buf,len);
    }
    dictReleaseIterator(di);
}

/* Build the message header */
void clusterBuildMessageHdr(clusterMsg *hdr, int type) {
    int totlen = 0;
    memset(hdr,0,sizeof(*hdr));
    hdr->sig[0] = 'R';
    hdr->sig[1] = 'C';
    hdr->sig[2] = 'm';
    hdr->sig[3] = 'b';
    hdr->type = htons(type);
    memcpy(hdr->sender,myself->name,PREZ_CLUSTER_NAMELEN);
    hdr->port = htons(server.port);
    /* Compute the message length for certain messages. */
    if (type == CLUSTERMSG_TYPE_VOTEREQUEST) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataRequestVote);
   } else if (type == CLUSTERMSG_TYPE_VOTEREQUEST_RESP) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataResponseVote);
    } else if (type == CLUSTERMSG_TYPE_APPENDENTRIES_RESP) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataResponseAppendEntries);
    }

    hdr->totlen = htonl(totlen);
    /* For CLUSTERMSG_TYPE_APPENDENTRIES, fixing the totlen field is up to the caller. */
}

// 接收到candidate的投票请求
void clusterProcessRequestVote(clusterLink *link, clusterMsgDataRequestVote vote) {
    sds candidateid = sdsnew(vote.candidateid);
    long long last_log_index, last_log_term;

    if (vote.term < server.cluster->current_term) {
        prezLog(PREZ_DEBUG, "RV Recv Req: Deny Vote, old term: %lld",
                vote.term);
        goto deny_vote;
    }

    if (vote.term > server.cluster->current_term) { // 如果投票的周期比服务器周期要大
        prezLog(PREZ_DEBUG, "RV Recv Req: Update term to: %lld",
                vote.term);
        server.cluster->state = PREZ_FOLLOWER;
        server.cluster->current_term = vote.term;
        server.cluster->leader = sdsempty();
        server.cluster->voted_for = sdsempty();

    } else if (sdslen(server.cluster->voted_for) > 0 &&
            sdscmp(server.cluster->voted_for, candidateid)) {
        prezLog(PREZ_DEBUG, "RV Recv Req: Deny Vote, Dup vote request."
                "Already voted for %s",
                server.cluster->voted_for);
        goto deny_vote;
    }

    last_log_index = logCurrentIndex();
    last_log_term = logCurrentTerm();
    if (last_log_index > vote.last_log_index ||
            last_log_term > vote.last_log_term) {
        prezLog(PREZ_DEBUG, "RV Recv Req: Deny Vote. Out of date log");
        goto deny_vote;
    }

    /* Vote for the candidate */
    server.cluster->voted_for = candidateid;
    prezLog(PREZ_DEBUG, "RV Recv Req: Grant Vote for %s.", candidateid);
    clusterSendResponseVote(link, GRANT_VOTE);
    server.cluster->last_activity_time = mstime();
    return;

deny_vote:
    clusterSendResponseVote(link, DENY_VOTE);
    sdsfree(candidateid);
    return;
}

void clusterProcessResponseVote(clusterLink *link,
        clusterMsgDataResponseVote vote) {

    if (vote.vote_granted && vote.term == server.cluster->current_term) {
        server.cluster->votes_granted++;
        return;
    }

    if (vote.term > server.cluster->current_term) {
        prezLog(PREZ_DEBUG, "RV Recv Rep: "
                "vote failed: updating term:%lld", vote.term);
        server.cluster->state = PREZ_FOLLOWER;
        server.cluster->current_term = vote.term;
        server.cluster->leader = sdsempty();
        server.cluster->voted_for = sdsempty();
    } else {
        prezLog(PREZ_DEBUG, "RV Recv Rep: vote denied");
    }
}

void clusterProcessAppendEntries(clusterLink *link,
        clusterMsgDataAppendEntries entries) {

    if (entries.term < server.cluster->current_term) {
        prezLog(PREZ_DEBUG, "AE Recv Req: Out of date term");
        clusterSendResponseAppendEntries(link, PREZ_ERR);
        return;
    }
    server.cluster->last_activity_time = mstime();

    if (entries.term == server.cluster->current_term) {
        if (server.cluster->state == PREZ_CANDIDATE) {
            server.cluster->state = PREZ_FOLLOWER;
        }
        server.cluster->leader = zstrdup(entries.leaderid);
    } else {
        server.cluster->state = PREZ_FOLLOWER;
        server.cluster->current_term = entries.term;
        server.cluster->leader = zstrdup(entries.leaderid);
        server.cluster->voted_for = sdsempty();
    }

    if (logVerifyAppend(entries.prev_log_index, entries.prev_log_term)) {
        prezLog(PREZ_DEBUG, "AE Recv Req: log verify error");
        clusterSendResponseAppendEntries(link, PREZ_ERR);
        return;
    }

    if (logAppendEntries(entries)) {
        prezLog(PREZ_DEBUG, "AE Recv Req: log append entries error");
        clusterSendResponseAppendEntries(link, PREZ_ERR);
        return;
    }

    if (logCommitIndex(entries.leader_commit_index)) {
        prezLog(PREZ_DEBUG, "AE Recv Req: log commit entries error");
        clusterSendResponseAppendEntries(link, PREZ_ERR);
        return;
    }

    clusterSendResponseAppendEntries(link, PREZ_OK);
}

void clusterProcessResponseAppendEntries(clusterLink *link,
        clusterMsgDataResponseAppendEntries entries) {
    clusterNode *node = link->node;

    if (entries.ok == PREZ_OK) {
        if (node->last_sent_entry) {
            node->next_index = node->last_sent_entry->log_entry.index+1;
            node->match_index = node->last_sent_entry->log_entry.index;
        }
    } else {
        if (entries.term > server.cluster->current_term) {
            prezLog(PREZ_NOTICE, "AE Recv Rep: New Leader found");
            server.cluster->state = PREZ_FOLLOWER;
            server.cluster->current_term = entries.term;
            server.cluster->leader = sdsempty();
            server.cluster->voted_for = sdsempty();
        } else {
            node->next_index--;
            prezLog(PREZ_DEBUG, "AE Recv Rep: "
                    "next_index--: %lld updated for %s",
                    node->next_index, node->name);
        }
    }
}

void clusterSendRequestVote(void) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    long long last_log_index, last_log_term;

    last_log_index = logCurrentIndex();
    last_log_term = logCurrentTerm();

    clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_VOTEREQUEST);
    server.cluster->current_term++;
    hdr->data.requestvote.vote.term = server.cluster->current_term;
    memcpy(hdr->data.requestvote.vote.candidateid, myself->name,
            PREZ_CLUSTER_NAMELEN);
    hdr->data.requestvote.vote.last_log_index = last_log_index;
    hdr->data.requestvote.vote.last_log_term = last_log_term;
    prezLog(PREZ_DEBUG, "RV Send Req: broadcast term: %lld",
            server.cluster->current_term);

    clusterBroadcastMessage(buf,ntohl(hdr->totlen));
}

void clusterSendResponseVote(clusterLink *link, int vote_granted) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;

    clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_VOTEREQUEST_RESP);
    prezLog(PREZ_DEBUG, "RV Send Rep: term:%lld, granted:%d",
            server.cluster->current_term, vote_granted);

    hdr->data.responsevote.vote.term = server.cluster->current_term;
    hdr->data.responsevote.vote.vote_granted = vote_granted;

    clusterSendMessage(link,buf,ntohl(hdr->totlen));
}

void clusterSendHeartbeat(clusterLink *link) {
    clusterSendAppendEntries(link);
}

void clusterSendResponseAppendEntries(clusterLink *link, int ok) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr =  (clusterMsg*) buf;

    clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_APPENDENTRIES_RESP);
    hdr->data.responseappendentries.entries.term =
        server.cluster->current_term;
    //FIXME: probably not needed
    hdr->data.responseappendentries.entries.index =
        logCurrentIndex();
    hdr->data.responseappendentries.entries.commit_index =
        server.cluster->commit_index;
    hdr->data.responseappendentries.entries.ok = ok;
    prezLog(PREZ_DEBUG, "AE Send Rep: term:%lld, idx:%lld,"
            " cmtidx:%lld, ok:%d",
            hdr->data.responseappendentries.entries.term,
            hdr->data.responseappendentries.entries.index,
            hdr->data.responseappendentries.entries.commit_index,
            hdr->data.responseappendentries.entries.ok);

    clusterSendMessage(link,buf,ntohl(hdr->totlen));
}

// 发送心跳给follower
void clusterSendAppendEntries(clusterLink *link) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr =  (clusterMsg*) buf;
    clusterNode *node = link->node;
    listNode *ln, *ln_next;
    logEntryNode *le_node;
    int logcount = 0, totlen;

    clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_APPENDENTRIES);
    // 当前leader的期限
    hdr->data.appendentries.entries.term = server.cluster->current_term;
    // leader的名字
    memcpy(hdr->data.appendentries.entries.leaderid, myself->name,
            PREZ_CLUSTER_NAMELEN);
    // 当前follower最后更新的日志index
    hdr->data.appendentries.entries.prev_log_index = node->next_index-1;
    hdr->data.appendentries.entries.prev_log_term = logGetTerm(node->next_index-1);
    hdr->data.appendentries.entries.leader_commit_index =
        server.cluster->commit_index;
    node->last_sent_entry = NULL;

    if (logCurrentIndex() >= node->next_index) {
        ln = getLogNode(node->next_index);
        while(ln && logcount < server.cluster->log_max_entries_per_request) {
            le_node = listNodeValue(ln);
            hdr->data.appendentries.entries.log_entries[logcount].term =
                le_node->log_entry.term;
            hdr->data.appendentries.entries.log_entries[logcount].index =
                le_node->log_entry.index;
            memcpy(hdr->data.appendentries.entries.log_entries[logcount].commandName,
                    le_node->log_entry.commandName,
                    strlen(le_node->log_entry.commandName));
            memcpy(hdr->data.appendentries.entries.log_entries[logcount].command,
                    le_node->log_entry.command,
                    strlen(le_node->log_entry.command));
            prezLog(PREZ_DEBUG,"AE Send Req: term:%lld, idx:%lld, cmd:%s, cmd:%s",
                    hdr->data.appendentries.entries.log_entries[logcount].term,
                    hdr->data.appendentries.entries.log_entries[logcount].index,
                    hdr->data.appendentries.entries.log_entries[logcount].commandName,
                    hdr->data.appendentries.entries.log_entries[logcount].command);

            ln_next = listNextNode(ln);
            logcount++;
            node->last_sent_entry = listNodeValue(ln);
            ln = ln_next;
        }
    }
    hdr->data.appendentries.entries.log_entries_count = htons(logcount);

    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += (sizeof(clusterMsgDataAppendEntries) -
            (sizeof(logEntry)*PREZ_LOG_MAX_ENTRIES_PER_REQUEST));
    totlen += (sizeof(logEntry)*logcount);
    hdr->totlen = htonl(totlen);

    prezLog(PREZ_DEBUG, "AE Send Req: %s, logcount: %d, totlen: %d",
            node->name,
            ntohs(hdr->data.appendentries.entries.log_entries_count),
            totlen);

    clusterSendMessage(link,buf,ntohl(hdr->totlen));
}

void clusterUpdateCommitIndex(void) {
    dictIterator *di;
    dictEntry *de;
    long long commit_index, *log_indices;
    int i=0;

    /* Committing log index by counting replicas is done only for log
     * index in current term and not for previous terms. This means
     * that when all nodes are shut and restarted, then the current
     * leader needs to receive atleast one request so that logCommitIndex
     * can happen for the entry in current term which in turn will trigger
     * commit for previous entries. After this, previous entries will be
     * available for clients to query */
    di = dictGetSafeIterator(server.cluster->nodes);
    log_indices = zmalloc(sizeof(long long)*
            dictSize(server.cluster->nodes));
    while((de = dictNext(di)) != NULL) {
        clusterNode *cnode = dictGetVal(de);
        log_indices[i++] = cnode->match_index;
    }
    dictReleaseIterator(di);
    qsort(log_indices,dictSize(server.cluster->nodes),
            sizeof(long long),
            compareIndices);
    reverseIndices(log_indices,dictSize(server.cluster->nodes));
    commit_index = log_indices[quorumSize-1];
    if (commit_index > server.cluster->commit_index &&
        server.cluster->current_term == logGetTerm(commit_index))
    {
        logSync();
        server.cluster->commit_index = commit_index;
        prezLog(PREZ_DEBUG, "Upd cmtidx: %lld",
                commit_index);
    }
    zfree(log_indices);
}

void clusterDoBeforeSleep(int flags)
{
    return;
}

/* -----------------------------------------------------------------------------
 * CLUSTER cron job
 * -------------------------------------------------------------------------- */

void clusterCron(void) {
    mstime_t now = mstime();
    mstime_t election_timeout;
    long long last_log_index;
    dictIterator *di;
    dictEntry *de;

    /* Check if we have disconnected nodes and re-establish the connection. */
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node->flags & (PREZ_NODE_MYSELF|PREZ_NODE_NOADDR)) continue;
        if (node->link == NULL) { // 连接所有节点
            int fd;
            clusterLink *link;

            fd = anetTcpNonBlockBindConnect(server.neterr, node->ip,
                    node->port+PREZ_CLUSTER_PORT_INCR, PREZ_BIND_ADDR);
            if (fd == -1) {
                prezLog(PREZ_WARNING, "Unable to connect to "
                        "Cluster Node [%s]:%d -> %s", node->ip,
                        node->port+PREZ_CLUSTER_PORT_INCR,
                        server.neterr);
                continue;
            }
            link = createClusterLink(node);
            link->fd = fd;
            node->link = link;
            node->last_activity_time = mstime();
            aeCreateFileEvent(server.el,link->fd,AE_READABLE,
                    clusterReadHandler,link);

            prezLog(PREZ_DEBUG,"Connecting with Node %.40s at %s:%d",
                    node->name, node->ip, node->port+PREZ_CLUSTER_PORT_INCR);
        }
    }
    dictReleaseIterator(di);

    /* Apply to state machine if possible */
    while (server.cluster->commit_index > server.cluster->last_applied) {
        server.cluster->last_applied++;
        logApply(server.cluster->last_applied);
    }

    election_timeout = server.cluster->election_timeout + /* Fixed delay. */
        random() % server.cluster->election_timeout; /* Random delay between 0
                                                        and election_timeout ms */

    // 候选人定时器超时
    if (server.cluster->state != PREZ_LEADER &&
            now - server.cluster->last_activity_time > election_timeout) {
        server.cluster->last_activity_time = mstime();
        prezLog(PREZ_NOTICE, "Changing State to Candidate, term: %lld",
                server.cluster->current_term);

        /* Change to Candidate State */
        server.cluster->state = PREZ_CANDIDATE;
        server.cluster->leader = sdsempty();

        /* Vote for self */
        server.cluster->voted_for = zstrdup(server.name);
        server.cluster->votes_granted = 1;

        /* Build Request Vote and Broadcast */
        clusterSendRequestVote();
    }

    /* Candidate */
    if (server.cluster->state == PREZ_CANDIDATE) {
        if (server.cluster->votes_granted >= quorumSize) { // 如果超过一半的节点投票, 那么成为leader
            prezLog(PREZ_DEBUG, "nodes/quorum: %lu/%lu, "
                    "Changing State to Leader",
                    dictSize(server.cluster->nodes), quorumSize);
            prezLog(PREZ_NOTICE, "Changing State to Leader, term: %lld",
                   server.cluster->current_term);
            server.cluster->state = PREZ_LEADER;
            server.cluster->leader = zstrdup(server.name);

            last_log_index = logCurrentIndex();

            di = dictGetSafeIterator(server.cluster->nodes);
            while((de = dictNext(di)) != NULL) { // 更新所以从节点信息
                clusterNode *node = dictGetVal(de);

                if (node->flags & (PREZ_NODE_MYSELF|PREZ_NODE_NOADDR)) continue;
                node->next_index = last_log_index+1;
                node->match_index = 0;
            }
        }
    }

    /* Leader */
    // Send heartbeat to all peers
    if (server.cluster->state == PREZ_LEADER) {
        clusterUpdateCommitIndex();
        di = dictGetSafeIterator(server.cluster->nodes);
        while((de = dictNext(di)) != NULL) {
            clusterNode *node = dictGetVal(de);

            if (node->flags & (PREZ_NODE_MYSELF|PREZ_NODE_NOADDR)) continue;
            if (node->link == NULL) continue;

            if (mstime() - node->last_activity_time >
                    server.cluster->heartbeat_interval) { // 如果到达发生心跳的时候
                node->last_activity_time = mstime();
                clusterSendHeartbeat(node->link); // 发送心跳包给从节点
            }
        }
        dictReleaseIterator(di);
    }
}
