# xv6-riscv
## 目的
xv6にはネットワーク機能が実装されていない。
本プロジェクトではxv6でパケットの送受信を行うことを目標とした。

## 利用ガイド
ネットワーク機能はファイルディスクリプタによって抽象化されている。

## デバイスファイルのオープン
rootディレクトリ中にnetというデバイスファイルが存在している。
システムコールopenなどを使ってnetファイルをオープンする。

## パケットの送信
netファイルオープン時にO\_WRONLYなどの適切なフラグを付与してやる。
パケットの送信はwriteシステムコールを用いて、netファイルに書き込みを行うことによって実現される。
```c
int fd = open("net", O_WRONLY);
if (fd < 0) {
    fprintf(2, "failed to open net");
    exit(-1);
}
char buf[512];
int n;
while ((n = read(0, buf, 512)) > 0) {
    write(fd, buf, n);
}
close(fd);
```

## パケットの受信
netファイルオープン時にO\_RDONLYなどの適切なフラグを付与してやる。
パケットの受信はreadシステムコールを用いて、netファイルから読み込みを行うことによって実現される。
1回の読み込みで1つのパケットを得ることができ、読み込んでいないパケットが存在しない場合は、新たなパケットを受信するまでブロッキングされる。
```c
int fd = open("net", O_RDONLY);
if (fd < 0) {
    fprintf(2, "failed to open net");
    exit(-1);
}
char buf[512];
int n = read(fd, buf, 512);
if (n > 0) {
    write(0, buf, n);
}
close(fd);
```

## テスト
パケットの受信は以下のコマンドでARPパケットを飛ばすことにより確認できる。
```bash
curl localhost:1234
```
また、パケットの送信はwiresharkなどでpcapファイルを解析することによって確かめることができる。
```bash
wireshark dump.pcap
```

また、いくつかのテストプログラムを用意している。

### テストプログラム: netwrite
netwriteはユーザーが入力した内容をそのまま送信するコマンドである。
$cat > net$と同等の処理を行う。

### テストプログラム: netread
netreadは受信したパケットを1つ読み込み出力するコマンドである。

### テストプログラム: netecho
netechoは受信したパケットを1つ読み込み、そのまま送信するコマンドである。

