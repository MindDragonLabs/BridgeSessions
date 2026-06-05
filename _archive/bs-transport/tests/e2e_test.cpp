// e2e_test.cpp — E2E: full stack relay from TLS handshake through PTY output
// POSIX-only: uses fork/posix_openpt (requires bs-server PTY infrastructure)
#ifndef _WIN32
#include <catch2/catch_test_macros.hpp>
#include "bstransport/tls.hpp"
#include "bstransport/frame_io.hpp"
#include "bsprotocol/codec.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <thread>
#include <fstream>
#include <cstdio>
#include <atomic>

using namespace bs::protocol;
using namespace bs::transport;

namespace {
struct TmpFile {
    std::string path;
    TmpFile(const std::string& content) {
        char tmpl[] = "/tmp/bs-e2e_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0) { ::write(fd, content.data(), content.size()); ::close(fd); path = tmpl; }
    }
    ~TmpFile() { if (!path.empty()) std::remove(path.c_str()); }
};
auto ck() { return generate_cert_key_pair("e2e.bs"); }
}

TEST_CASE("E2E: TLS + Attach + PTY spawn + Output relay", "[e2e]") {
    auto [sc,sk] = ck(); auto [cc,ck_priv] = ck();
    auto ch = pubkey_hex_from_pem(ck_priv);
    TmpFile sf(sc), kf(sk), cf(cc), cfk(ck_priv), ak(ch+"\n"), ks("");

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt=1; setsockopt(lfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    bind(lfd,(sockaddr*)&a,sizeof(a)); listen(lfd,1);
    socklen_t al=sizeof(a); getsockname(lfd,(sockaddr*)&a,&al); int port=ntohs(a.sin_port);

    std::string relayed;

    // Server: accept → TLS → read Attach → spawn echo → write Output → close
    std::jthread srv([&]{
        int cfd=accept(lfd,nullptr,nullptr); if(cfd<0)return;
        ServerConfig scfg{sf.path,kf.path,ak.path}; auto ctx=create_server_context(scfg);
        auto s=SslPtr(SSL_new(ctx.get())); SSL_set_fd(s.get(),cfd); SSL_accept(s.get());

        auto m=read_frame(s.get());
        auto* at=std::get_if<AttachMsg>(&m); REQUIRE(at);

        // Spawn bash -c "echo E2E_OK"
        int master=posix_openpt(O_RDWR|O_NOCTTY); REQUIRE(master>=0);
        grantpt(master); unlockpt(master);
        pid_t c=fork();
        if(c==0){ setsid(); int sl=open(ptsname(master),O_RDWR);
                  dup2(sl,0);dup2(sl,1);dup2(sl,2); if(sl>2)close(sl);
                  setenv("TERM","xterm-256color",1);
                  execlp("bash","bash","-c","echo E2E_OK; exit 0",nullptr); _exit(127); }

        // Read PTY output
        char buf[65536];
        for(int i=0;i<20;i++){
            ssize_t n=read(master,buf,sizeof(buf)-1);
            if(n>0){buf[n]=0; relayed+=buf; if(relayed.find("E2E_OK")!=std::string::npos) break;}
            usleep(100000);
        }
        OutputMsg out; out.data=relayed;
        write_frame(s.get(),out);
        waitpid(c,nullptr,0); close(master); close(cfd);
    });

    // Client: connect → TLS → Attach → read Output
    int cfd=socket(AF_INET,SOCK_STREAM,0);
    sockaddr_in ca{}; ca.sin_family=AF_INET; ca.sin_addr.s_addr=htonl(INADDR_LOOPBACK); ca.sin_port=htons(port);
    connect(cfd,(sockaddr*)&ca,sizeof(ca));
    ClientConfig ccfg{cf.path,cfk.path,ks.path};
    auto ctx=create_client_context(ccfg,[](auto&){return true;});
    auto ssl=SslPtr(SSL_new(ctx.get())); SSL_set_fd(ssl.get(),cfd); SSL_connect(ssl.get());

    AttachMsg at; at.session_name="e2e"; write_frame(ssl.get(),at);
    auto reply=read_frame(ssl.get());
    auto* out=std::get_if<OutputMsg>(&reply);
    REQUIRE(out!=nullptr);
    REQUIRE(out->data.find("E2E_OK")!=std::string::npos);

    SSL_shutdown(ssl.get()); close(cfd); close(lfd);
    INFO("E2E relayed: "<<relayed);
}
#endif // _WIN32
