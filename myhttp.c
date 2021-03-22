#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<sys/wait.h>
#include<sys/types.h>
#include<sys/stat.h>
#include<sys/epoll.h>
#include<unistd.h>
#include<fcntl.h>

#define MAXSIZE 2048

void sys_err(char *str)
{
	perror(str);
	exit(1);
}

int get_line(int cfd,char *buf,int size)
{
	int i=0;
	char c='\0';
	int n;
	while ((i<size-1) && (c!='\n'))
	{
		n=recv(cfd,&c,1,0);
		if (n>0)
		{
			if (c=='\r')
			{
				n=recv(cfd,&c,1,MSG_PEEK);
				if ((n>0) && (c=='\n'))
				{
					recv(cfd,&c,1,0);
				}
				else
					c='\n';
			}
			buf[i]=c;
			i++;
		}
		else 
			c='\n';
	}	
	buf[i]='\0';
	if (n==-1) i=n;
	return i;
}


void disconnect(int cfd,int epfd)
{
	int ret=epoll_ctl(epfd,EPOLL_CTL_DEL,cfd,NULL);
	if (ret==-1) sys_err("epoll_ctl_del error");
}


void send_respond(int cfd,int no,char *disp,char *type,int len)
{
	char buf[4096]={0};
	
	sprintf(buf,"HTTP/1.1 %d %s\r\n",no,disp);
	send(cfd,buf,strlen(buf),0);
	
	sprintf(buf,"Content-Type: %s\r\n",type);
	sprintf(buf+strlen(buf),"Content-Length:%d\r\n",len);
	send(cfd,buf,strlen(buf),0);
	
	send(cfd,"\r\n",2,0);
}

void send_file(int cfd,const char *file)
{
	int n=0,ret;
	char buf[4096]={0};
	
	int fd=open(file,O_RDONLY);
	if (fd==-1) sys_err("open error");
	
	while ((n=read(fd,buf,sizeof(buf)))>0)
	{
		ret=send(cfd,buf,n,0);
		if (ret==-1) sys_err("send error");
		
		if (ret<4096) printf("------send ret: %d\n",ret);
	}
	close(fd);
}




void http_request(int cfd,const char*file)
{
	struct stat sbuf;
	int ret=stat(file,&sbuf);
	if (ret!=0) sys_err("stat");
	
	if (S_ISREG(sbuf.st_mode))
	{
		send_respond(cfd,200,"OK","Content-Type:image/jpeg",-1);
		send_file(cfd,file);
	}
}


int init_listen_fd(int port,int epfd)
{
	int lfd=socket(AF_INET,SOCK_STREAM,0);
	if (lfd==-1) sys_err("socket error");
	
	struct sockaddr_in srv_addr;
	bzero(&srv_addr,sizeof(srv_addr));
	srv_addr.sin_family=AF_INET;
	srv_addr.sin_port=htons(port);
	srv_addr.sin_addr.s_addr=htonl(INADDR_ANY);
	
	int opt=1;
	setsockopt(lfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
	
	int ret=bind(lfd,(struct sockaddr*)&srv_addr,sizeof(srv_addr));
	if (ret==-1) sys_err("bind errror");
	
	ret=listen(lfd,128);
	if (ret==-1) sys_err("listen error");
	
	struct epoll_event ev;
	ev.events=EPOLLIN;
	ev.data.fd=lfd;
	
	ret=epoll_ctl(epfd,EPOLL_CTL_ADD,lfd,&ev);
	if (ret==-1) sys_err("epoll_ctl error");
	
	return lfd;
}

void do_accept(int lfd,int epfd)
{
	struct sockaddr_in clt_addr;
	socklen_t clt_addr_len=sizeof(clt_addr);
	
	int cfd=accept(lfd,(struct sockaddr*)&clt_addr,&clt_addr_len);
	if (cfd==-1) sys_err("accept error");
	
	char client_ip[64]={0};
	printf("New client IP: %s,Port: %d, cfd= %d\n",
		inet_ntop(AF_INET,&clt_addr.sin_addr.s_addr,client_ip,sizeof(client_ip)),
		ntohs(clt_addr.sin_port),
		cfd);
	
	int flag=fcntl(cfd,F_GETFL);
	flag |=O_NONBLOCK;
	fcntl(cfd,F_SETFL,flag);
	
	struct epoll_event ev;
	ev.data.fd=cfd;
	ev.events=EPOLLIN | EPOLLET;
	
	int ret=epoll_ctl(epfd,EPOLL_CTL_ADD,cfd,&ev);
	if (ret==-1) sys_err("epoll_ctl_add cfd error");
}

void do_read(int cfd,int epfd)
{
	char line[1024]={0};
	char method[16],path[256],protocol[16];
	
	int len=get_line(cfd,line,sizeof(line));
	if (len==0)
	{
		printf("client was closed already\n");
		disconnect(cfd,epfd);
	}
	else
	{
		sscanf(line,"%[^ ] %[^ ] %[^ ]",method,path,protocol);
		printf("method=%s,path=%s,protocol=%s\n",method,path,protocol);
		
		while (1)
		{
			char buf[1024]={0};
			len=get_line(cfd,buf,sizeof(buf));
			if (buf[0]=='\n') 
				break;
			else if (len==1)
				break;
		}
	}
	
	if (strncasecmp(method,"GET",3)==0)
	{
		char *file=path+1;
		http_request(cfd,file);
		disconnect(cfd,epfd);
	}
}

void epoll_run(int port)
{
	int i=0;
	struct epoll_event all_events[MAXSIZE];
	
	int epfd=epoll_create(MAXSIZE);
	if (epfd==-1) sys_err("epoll_create error");
	
	int lfd=init_listen_fd(port,epfd);
	
	while (1)
	{
		int ret=epoll_wait(epfd,all_events,MAXSIZE,-1);
		if (ret==-1) sys_err("epoll_wait error");
		
		for (i=0;i<ret;i++)
		{
			struct epoll_event *pev=&all_events[i];
			if (!(pev->events &EPOLLIN)) continue;
			if (pev->data.fd==lfd)
				do_accept(lfd,epfd);
			else
				do_read(pev->data.fd,epfd);
		}
	}
}


int main(int argc,char * argv[])
{
	if (argc<3) printf("./server port path\n");
	
	int port=atoi(argv[1]);
	
	int ret=chdir(argv[2]);
	if (ret!=0) sys_err("chdir error");
	
	epoll_run(port);
	
	return 0;
}
	





			
	















