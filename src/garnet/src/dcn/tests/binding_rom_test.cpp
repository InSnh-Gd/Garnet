#include "dp_binding.h"
#include "atombios.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc,char **argv) {
    if(argc!=2) return 2;
    FILE *f=std::fopen(argv[1],"rb"); if(!f) return 2;
    std::fseek(f,0,SEEK_END); const long length=std::ftell(f); std::rewind(f);
    if(length<=0) return 2;
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    if(std::fread(bytes.data(),1,bytes.size(),f)!=bytes.size()) return 2;
    std::fclose(f);
    AtomBios bios;
    if(!bios.init(bytes.data(),bytes.size())) return 3;
    AtomBios::DisplayPath paths[AtomBios::MaxDisplayPaths];
    const size_t count=bios.getDisplayPaths(paths,AtomBios::MaxDisplayPaths);
    unsigned direct=0;
    for(size_t i=0;i<count;++i) {
        const auto &p=paths[i];
        const bool proof=rdna4_binding_direct_path(bytes.data(),bytes.size(),bios.dataTable(AtomBios::DisplayObjectInfo),p.connectorObjId,p.encoderObjId);
        const uint32_t be=rdna4_binding_encoder(p.encoderObjId);
        std::printf("ROM path %zu connector=%#x encoder=%#x direct-v1.5=%u legacy-BE=%u\n",i,p.connectorObjId,p.encoderObjId,proof,be);
        if(AtomBios::connectorType(p.connectorObjId)==AtomBios::ConnectorDP && proof && be<4) ++direct;
    }
    if(!direct) return 4;
    std::printf("binding real-ROM fixture: PASS (%u direct DP candidates; not RX9060XT hardware validation)\n",direct);
}
