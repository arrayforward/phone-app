#include "tight/tight.hpp"
#include "tight/blocking_queue.hpp"
#include "tight/logger.hpp"
#include "tight/types.hpp"

#include "net/address.hpp"
#include "util/buffer_pool.hpp"
#include "channel/command.hpp"
#include "crypto/crypto.hpp"
#include "net/ecn_platform.hpp"
#include "channel/fragmenter.hpp"
#include "core/peer.hpp"
#include "channel/reassembler.hpp"
#include "channel/report.hpp"
#include "util/small_thread.hpp"
#include "net/socket_platform.hpp"
#include "protocol/wire_format.hpp"
#include "protocol/wire_format.hpp"
#include "net/wsa.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "util/dbg_log.hpp"

namespace tight {

using namespace tight_detail;

class TightTransport::Impl {
public:
    TightConfig m_config;
    NativeSocket m_sock{kInvalidSocket};
    // 外部 socket 模式（见 socket.hpp）：非空时 tight 不创建/绑定/关闭
    // socket，也不启动接收线程——发送经 send_to() 输出，接收由应用
    // inject_packet() 注入。
    std::shared_ptr<ISocket> m_ext_socket;
    std::uint16_t m_local_port{};
    std::atomic<bool> m_running{false};
    SmallThread m_reactor_thread;

    // 绮剧畝妯″紡鍙繍琛屾椂鍔ㄦ€佸垏鎹細m_lite_mode=true 锟?reactor 鍚堝苟
    // encode/sender 鑱岃矗锟? 绾跨▼锛夛紱false 鏃剁嫭绔嬪伐浣滅嚎绋嬶紙4 绾跨▼锛夛拷?
    // m_workers_running 鍗曠嫭鎺у埗 encode/sender 绾跨▼鐢熷懡鍛ㄦ湡锟?
    // m_workers_mutex 涓茶锟?spawn/join锟?
    std::atomic<bool> m_lite_mode{false};
    std::atomic<bool> m_workers_running{false};
    std::mutex m_workers_mutex;

    // 绮剧畝妯″紡绾跨▼浣跨敤 64KB 灏忔爤锛坰td::thread 榛樿 1MB 棰勭暀锛夛紱
    // 瀹屾暣妯″紡浣跨敤绯荤粺榛樿锟?
    std::size_t thread_stack() const {
        return m_lite_mode.load() ? 64 * 1024 : 0;
    }

    mutable std::mutex m_send_mutex;
    struct SendMsg {
        std::string m_peer;
        Bytes m_payload;
        std::uint8_t m_channel{0};
        bool m_keyframe{false};   // send_video 的 IDR 标记（穿透到线上 reserved bit11）
    };
    std::map<int, std::deque<SendMsg>> m_send_queue;

    mutable std::mutex m_peers_mutex;

    // Worker thread for CPU-bound fragment encoding (FEC + RS).
    // The reactor stays free to process incoming packets.
    struct EncodeTask {
        Peer* m_peer;
        Bytes m_payload;
        std::uint8_t m_channel{0};
        bool m_keyframe{false};
    };
    BlockingQueue<EncodeTask> m_encode_queue;
    SmallThread m_encode_thread;
    // file 閫氶亾鍙戦€佺 file_id 鍒嗛厤鍣紙閫掑锛屾帴鏀剁鎸夋鍖哄垎鏂囦欢涓婁笅鏂囷級锟?
    std::uint32_t m_file_id_out{1};

    // Single-producer-multiple-consumer outbound packet queue.
    // The sender thread drains this and calls ::sendto; reactor and encode
    // thread only enqueue. This guarantees sendto (and any token-bucket
    // back-pressure) never blocks the reactor or the encode thread.
    struct OutboundPacket {
        Peer* m_peer;
        std::uint8_t m_channel{0};
        PooledBytes m_datagram;   // 姹犲寲缂撳啿锛坱hread_local 鍧楁睜锛屾棤閿佸鐢級
    };
    BlockingQueue<OutboundPacket> m_outbound_queue;
    // 音频通道（channel==1）独立出站队列：sender 优先清空且绕过令牌桶
    // （实时音频无条件一次性发完，不受贷款/令牌限制）。容量 128（≈1.2s
    // 音频，20ms 仅 2 包），满则静默丢（与普通队列一致）。
    BlockingQueue<OutboundPacket> m_audio_queue;
    SmallThread m_sender_thread;
    // 按通道排空标记（unix ms 截止）：排空期内该通道的数据报在出队时
    // 直接丢弃（不占带宽、不发），其他通道不受影响。应用在积压止损/
    // 带宽骤降时调用 drain_channel() 设置，期满自动恢复。
    std::atomic<std::uint64_t> m_drain_until_ms[8]{};

    // file/data 通道（2/3）发送字节累计（send_raw 累计）+ 速率采样状态：
    // video_capacity_bps 计算时扣除该通道的实时发送速率。
    std::atomic<std::uint64_t> m_fd_tx_bytes{0};
    std::uint64_t m_fd_rate_last_bytes{0};
    std::chrono::steady_clock::time_point m_fd_rate_last_ts{};

    // 视频容量通知：变化迟滞（相对 >10% 且绝对 >100k）后经常驻通知线程
    // 回调应用（接收线程只做检测入队，不阻塞）。
    BlockingQueue<std::uint64_t> m_cap_queue{4};
    SmallThread m_cap_thread;
    std::uint64_t m_last_cap_notified{0};
    // 拥塞排空窗口状态（双模式，slowdown_window_ms > 0 时生效）：
    //   fast（降幅 >50%：×0.45/×0.30/×0.20）：清空视频积压（clear_outbound）
    //     + 通道排空 100ms + 回调应用重启编码器出新 IDR，播放端跳到新 IDR
    //     时间线，追赶/超发积压瞬间归零。排空窗口完成 = 新 IDR 提交发送 +
    //     下一报告到达（确认）；btl 冻结期独立于窗口状态 = 触发 +3s
    //     （排空 ~0.7s + 锁定补足——刚跳帧迟到信号失真，不能立即爬升）。
    //   slow（降幅 20%~50%：×0.65）：按 Q 面积法排空——触发时刻快照超发
    //     积压 Q（send−recv 积分）与 btl，窗口内 cap = (btl_snap − Q/窗口)，
    //     3s 排完积压后结束（Q 清零）。
    // 两种模式冻结期内 btl 完全冻结（on_report 全部信号豁免）。
    std::atomic<int> m_evac_mode{0};       // 0=无 1=fast 2=slow
    std::atomic<bool> m_evac_keyframe_sent{false};                 // fast：新 IDR 已提交发送
    std::atomic<std::uint64_t> m_evac_freeze_until_ns{0};          // fast：btl 冻结到期（触发+3s；IDR 提交时再延长 kKeyframeHoldTotal，
                                                                    // 关键帧超发 CE 豁免并入冻结期——IDR 60KB 低带宽传输 27-33ms 触达
                                                                    // l4s 阈值 → proxy CE → 误判真超发急降 → 又 fast 跳帧循环）
    std::atomic<bool> m_evac_reset_token{false};                   // fast：请求破产清算（token 债务清零 + 解除贷款排空，sender 线程处理）
    std::chrono::steady_clock::time_point m_evac_processed_cong{}; // 已处理的降速时刻（防重复触发）
    std::chrono::steady_clock::time_point m_evac_slow_deadline{};  // slow：3s 到期时刻
    std::uint64_t m_evac_slow_q_bytes{0};   // slow：Q 快照（超发积压量，bytes）
    std::uint64_t m_evac_slow_btl_snap{0};  // slow：触发时刻 btl 快照（B/s）
    static constexpr std::uint64_t kEvacDrainMs = 100;             // fast 通道排空时长（旧管线残留出队即丢）
    static constexpr auto kEvacTimeout = std::chrono::seconds(5);  // fast 兜底：应用未出 IDR 强制结束排空窗口
    static constexpr auto kEvacFreezeTotal = std::chrono::seconds(3);  // fast 冻结期 = 排空（~0.7s）+ 锁定补足
    static constexpr auto kKeyframeHoldTotal = std::chrono::milliseconds(800);  // 关键帧超发锁定：IDR 传输（~30ms）+ 报告确认（333ms）+ 余量
    static constexpr double kEvacFastMaxFactor = 0.60;             // 降幅 ≥35%（因子 ≤0.60：柔表 ×0.50/×0.65 档 = strength≥20% 重/严重拥塞）→ fast；降幅 25%（×0.75）→ slow；<20%（×0.90）不触发
    // 总发送字节（send_raw 累计，含全部通道/控制）：超发积压累计的数据源
    std::atomic<std::uint64_t> m_tx_bytes{0};
    std::uint64_t m_tx_rate_last_bytes{0};
    std::chrono::steady_clock::time_point m_tx_rate_last_ts{};
    // 持续超发判定 EWMA（send/recv 平滑，α=0.5）：防 IDR 关键帧突刺的
    // 报告窗口错位（send 计入窗口 A、recv 计入窗口 B）误判持续超发
    double m_overload_send_ewma{0.0};
    double m_overload_recv_ewma{0.0};
    // 超发积压累计（bytes）：每报告积分 max(0, 发送速率−对端接收速率)×Δt
    // （面积 = 超发量）。slow 排空（3 秒排空）窗口触发时快照、结束清零，
    // fast 排空（清队列）不需要（积压直接丢弃）。m_cap_mu 保护。
    std::uint64_t m_evac_pending{0};
    mutable std::mutex m_cap_mu;   // 保护 m_fd_rate_last_* 采样推进 / m_last_cap_notified 迟滞 / Q 积分
    TightTransport::VideoCapacityCallback m_video_capacity_cb;
    // 鏈€杩戜竴娆″簲鐢ㄦ暟鎹彂閫佹椂鍒伙紙unix ms锛夛細app_limited 鍒ゅ畾鐢ㄢ€斺€旇棰戠瓑
    // 鎸佺画娴佸湪甯ч棿绌洪殭闃熷垪鐭殏涓虹┖锛岃嫢锟?闃熷垪绌哄嵆 app_limited"鍒ゅ畾锟?
    // 鎶曢€掔巼鏍锋湰浼氳璺宠繃瀵艰嚧 btl 鍗℃锛涚敤 500ms 鏃犲彂閫佹椿鍔ㄦ墠瑙嗕负锟?
    // 搴旂敤鍙楅檺锟?
    // RTT 长期 >200ms 关闭 FEC 冗余的阈值（平滑 EWMA，见 handle_report）
    static constexpr std::uint32_t kFecDisableRttMs = 200;
    std::atomic<std::uint64_t> m_last_app_send_ms{0};
    // 绮剧畝妯″紡锛坙ite_mode锛変笅 reactor 鍚堝苟 sender 鑱岃矗鏃剁殑褰撳墠寰呭彂鎶ユ枃
    std::optional<OutboundPacket> m_lite_pending;

    // Dedicated receiver thread. Calls recvfrom + handle_packet.
    SmallThread m_receiver_thread;

    std::map<std::string, Peer> m_peers;
    std::map<AddrKey, std::string> m_peer_by_addr;

    std::uint32_t m_local_client_id{};
    std::uint64_t m_local_session_id{};

    double m_token_bucket{0};
    std::chrono::steady_clock::time_point m_token_bucket_time;
    // 令牌贷款：token 允许为负（视频透支），额度 = btl × loan_seconds。
    // 贷款耗尽 → m_loan_drain（视频通道持续排空至债务清零）+ 回调通知。
    std::atomic<bool> m_loan_drain{false};
    bool m_loan_exhausted_reported{false};   // 防重复回调（耗尽/恢复各一次）
    TightTransport::LoanExhaustedCallback m_loan_exhausted_cb;
    TightTransport::EvacKeyframeCallback m_evac_keyframe_cb;

    mutable std::mutex m_callback_mutex;
    TightTransport::MessageCallback m_message_cb;
    TightTransport::MessageLossCallback m_message_loss_cb;
    TightTransport::FileCallback m_file_cb;
    TightTransport::DataCallback m_data_cb;
    TightTransport::PeerCallback m_peer_cb;
    TightTransport::CommandCallback m_command_cb;

    BandwidthEstimator m_bandwidth;

    // 鏈湴 X25519 瀵嗛挜瀵癸紙ECDH锛夛紝鎻℃墜鏃朵氦鎹㈠叕閽ュ崗鍟嗕細璇濆瘑锟?
    X25519KeyPair m_local_keypair{};

    std::mt19937_64 m_rng;
    std::mutex m_rng_mutex;

    Impl(TightConfig cfg, std::shared_ptr<ISocket> ext_socket = nullptr)
        : m_config(std::move(cfg)),
          m_ext_socket(std::move(ext_socket)),
          // lite 队列收紧按业务画像（LiteProfile）：
          //   Audio：encode≤16、outbound≤32（音频 20ms 2 包 + 333ms 报告，
          //     32 条 ≈1s 余量——极致低内存）
          //   Video：encode≤64、outbound≤256（标准 lite，视频帧缓冲）
          m_encode_queue(
              m_config.lite_mode
                  ? (m_config.lite_profile == tight::LiteProfile::Audio
                         ? std::min<std::size_t>(m_config.encode_queue_limit, 16)
                         : std::min<std::size_t>(m_config.encode_queue_limit, 64))
                  : m_config.encode_queue_limit),
          m_outbound_queue(
              m_config.lite_mode
                  ? (m_config.lite_profile == tight::LiteProfile::Audio
                         ? std::min<std::size_t>(m_config.outbound_queue_limit, 32)
                         : std::min<std::size_t>(m_config.outbound_queue_limit, 256))
                  : m_config.outbound_queue_limit),
          m_audio_queue(m_config.lite_mode &&
                                m_config.lite_profile == tight::LiteProfile::Audio
                            ? 48   // 音频独立队列：48 条 ≈ 480ms（20ms 2 包）
                            : 128),
           m_bandwidth(m_config.initial_bandwidth_bytes) {
        // m_rng 榛樿鏋勯€犱細寰楀埌鍥哄畾搴忓垪锛氬悓绉掑惎鍔ㄧ殑澶氫釜杩涚▼灏嗕骇鐢熺浉鍚岀殑
        // client_id/session_id锛圙CM nonce 澶嶇敤闅愭偅锛夈€傜敤 random_device +
        // 楂樼簿搴︽椂閽熸贩鍚堟挱绉嶏紝淇濊瘉杩涚▼闂村簭鍒楁棤鍏筹拷?
        {
            std::random_device rd;
            std::uint64_t seed = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
            seed ^= static_cast<std::uint64_t>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count());
            seed ^= reinterpret_cast<std::uintptr_t>(this);
            m_rng.seed(seed);
        }
        m_local_client_id = static_cast<std::uint32_t>(random_u64() & 0x7FFFFFFFu);
        m_local_session_id = random_u64();
        if (m_local_client_id == 0) m_local_client_id = 1;
        m_token_bucket_time = std::chrono::steady_clock::now();
        m_lite_mode.store(m_config.lite_mode);
        if (m_config.encryption_enabled) {
            m_local_keypair = x25519_generate();
        }
    }
    std::size_t queue_limit() const {
        if (!m_lite_mode.load()) return m_config.queue_limit;
        // lite：Audio 进一步收紧（≤64 条消息 ≈ 音频 1.3s），Video ≤128
        if (m_config.lite_profile == tight::LiteProfile::Audio)
            return std::min<std::size_t>(m_config.queue_limit, 64);
        return std::min<std::size_t>(m_config.queue_limit, 128);
    }

    // 鍗曟潯娑堟伅鏈€澶ч暱搴︼細閰嶇疆鍊艰嚜鍔ㄩ挸鍒跺埌 [8KB, 10MB]
    std::size_t max_message_bytes() const {
        return std::max<std::size_t>(8 * 1024,
                   std::min<std::size_t>(m_config.max_message_bytes,
                                         10 * 1024 * 1024));
    }

    // 鎺掔┖鑺傛媿锛歭ite 妯″紡锛圛oT 璁惧锛夐挸鍒跺埌 锟?0ms锛岄檷锟?CPU 鍞ら啋棰戠巼
    // 锟?00锟?s 锟?100锟?s锛夛紝锟?锟?0ms 闄勫姞寤惰繜鎹㈠彇鍔燂拷?
    std::chrono::milliseconds flush_interval() const {
        return m_lite_mode.load()
                   ? std::max(m_config.flush_interval, std::chrono::milliseconds(10))
                   : m_config.flush_interval;
    }

    // 杩愯鏃跺垏鎹㈢簿绠€妯″紡锟? 绾跨▼ <-> 4 绾跨▼锛夛拷?
    // 闃熷垪瀹归噺鎸夋瀯閫犳椂閰嶇疆鍥哄畾锛屽垏鎹㈠彧鏀瑰彉绾跨▼妯″瀷锟?
    void set_lite_mode(bool lite) {
        std::lock_guard<std::mutex> lock(m_workers_mutex);
        if (lite == m_lite_mode.load()) return;
        // 涓㈠純鏃ュ織闅忔ā寮忓垏鎹細lite 闈欓粯锛屾櫘閫氭ā寮忔寜閰嶇疆鎭㈠
        {
            std::lock_guard<std::mutex> plock(m_peers_mutex);
            for (auto& kv : m_peers) {
                kv.second.m_drop_log = m_config.drop_log && !lite;
            }
        }
        if (lite) {
            // reactor 鍏堟帴绠″悎骞惰亴璐ｏ紙涓庡伐浣滅嚎绋嬪弻娑堣垂鍚屼竴闃熷垪/鍚屼竴
            // socket锛屽畨鍏級锛屽啀锟?receiver/encode/sender 閫€鍑哄苟 join
            m_lite_mode.store(true);
            m_workers_running.store(false);
            if (m_receiver_thread.joinable()) {
                try { m_receiver_thread.join(); } catch (...) {}
            }
            if (m_encode_thread.joinable()) {
                try { m_encode_thread.join(); } catch (...) {}
            }
            if (m_sender_thread.joinable()) {
                try { m_sender_thread.join(); } catch (...) {}
            }
        } else {
            // 鍏堟妸 reactor 妲戒綅涓殑寰呭彂鎶ユ枃鍙戞帀锛岄伩鍏嶅崌绾у悗婊炵暀
            if (m_lite_pending) {
                auto& pkt = *m_lite_pending;
                if (pkt.m_peer->m_addr_set && socket_ready()) {
                    socket_send(pkt.m_peer->m_addr, pkt.m_datagram.data(),
                                pkt.m_datagram.size());
                }
                m_lite_pending.reset();
            }
            // 鍏堝惎鍔ㄥ伐浣滅嚎绋嬪啀閫€鍑虹簿绠€鍚堝苟锛岄伩鍏嶉槦鍒楁棤浜烘秷锟?
            if (m_running.load()) {
                m_workers_running.store(true);
                if (!m_receiver_thread.joinable() && !m_ext_socket) {
                    m_receiver_thread = SmallThread([this] { receiver_loop(); }, 0);
                }
                if (!m_encode_thread.joinable()) {
                    m_encode_thread = SmallThread([this] { encode_loop(); }, 0);
                }
                if (!m_sender_thread.joinable()) {
                    m_sender_thread = SmallThread([this] { sender_loop(); }, 0);
                }
            }
            m_lite_mode.store(false);
        }
    }

    std::uint64_t random_u64() {
        std::lock_guard<std::mutex> lock(m_rng_mutex);
        if (m_rng() == 0 && (m_rng() == 0)) {
            std::random_device rd;
            return (static_cast<std::uint64_t>(rd()) << 32) | rd();
        }
        std::uint64_t a = m_rng();
        std::uint64_t b = m_rng();
        return a ^ (b << 1);
    }

    void set_message_callback(TightTransport::MessageCallback cb) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_message_cb = std::move(cb);
    }

    void set_peer_callback(TightTransport::PeerCallback cb) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_peer_cb = std::move(cb);
    }

    void set_command_callback(TightTransport::CommandCallback cb) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_command_cb = std::move(cb);
    }

    void set_message_loss_callback(TightTransport::MessageLossCallback cb) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_message_loss_cb = std::move(cb);
    }

    void set_file_callback(TightTransport::FileCallback cb) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_file_cb = std::move(cb);
    }

    void set_data_callback(TightTransport::DataCallback cb) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_data_cb = std::move(cb);
    }

    // ---- file/data 娑堟伅鏍煎紡锛堥€氶亾 2=file锟?=data锛屽潎鍙潬 ARQ锟?---
    // manifest: {0x01} file_id(4) name_len(2) name total(8) chunk_size(4) chunk_count(4)
    // chunk:    {0x02} file_id(4) idx(4) data
    // data:     {0x03} data
    static constexpr std::uint8_t kFileChannel = 2;
    static constexpr std::uint8_t kDataChannel = 3;
    static constexpr std::size_t kFileChunkSize = 60 * 1024;  // chunk payload (msg <= 64KB)

    void fire_file(Peer* peer, const std::string& name, Bytes data) {
        TightTransport::FileCallback cb;
        { std::lock_guard<std::mutex> lock(m_callback_mutex); cb = m_file_cb; }
        if (cb) cb(peer->m_id, name, std::move(data));
    }
    void fire_data_msg(Peer* peer, Bytes data) {
        TightTransport::DataCallback cb;
        { std::lock_guard<std::mutex> lock(m_callback_mutex); cb = m_data_cb; }
        if (cb) cb(peer->m_id, std::move(data));
    }

    // 鎺ユ敹绔細澶勭悊 file-manifest锛堝垵濮嬪寲鏂囦欢鎺ユ敹涓婁笅鏂囷級
    void handle_file_manifest(Peer* peer, const Bytes& p, std::size_t off) {
        if (p.size() < off + 4 + 2 + 8 + 4 + 4) return;
        std::uint32_t file_id = 0;
        std::memcpy(&file_id, p.data() + off, 4); file_id = to_be32(file_id); off += 4;
        std::uint16_t name_len = 0;
        std::memcpy(&name_len, p.data() + off, 2); name_len = to_be16(name_len); off += 2;
        if (p.size() < off + name_len) return;
        std::string name(reinterpret_cast<const char*>(p.data() + off), name_len); off += name_len;
        if (p.size() < off + 8 + 4 + 4) return;
        std::uint64_t total = 0;
        std::memcpy(&total, p.data() + off, 8); total = to_be64(total); off += 8;
        std::uint32_t chunk_size = 0, chunk_count = 0;
        std::memcpy(&chunk_size, p.data() + off, 4); chunk_size = to_be32(chunk_size); off += 4;
        std::memcpy(&chunk_count, p.data() + off, 4); chunk_count = to_be32(chunk_count);
        if (chunk_count > 65536) return;  // 闃插尽寮傚父鍧楁暟
        std::lock_guard<std::mutex> lk(peer->m_mu);
        auto& f = peer->m_files[file_id];
        f.name = std::move(name);
        f.total = total;
        f.chunk_size = chunk_size;
        f.chunk_count = chunk_count;
        f.chunks.assign(chunk_count, std::nullopt);
        f.received = 0;
    }

    // 鎺ユ敹绔細澶勭悊 file-chunk锛堝幓锟?+ 閲嶇粍锛屽叏閮ㄥ埌榻愪氦锟?on_file锟?
    void handle_file_chunk(Peer* peer, const Bytes& p, std::size_t off) {
        if (p.size() < off + 4 + 4) return;
        std::uint32_t file_id = 0, idx = 0;
        std::memcpy(&file_id, p.data() + off, 4); file_id = to_be32(file_id); off += 4;
        std::memcpy(&idx, p.data() + off, 4); idx = to_be32(idx); off += 4;
        Bytes chunk(p.begin() + off, p.end());
        FileRecv* f = nullptr;
        bool complete = false;
        {
            std::lock_guard<std::mutex> lk(peer->m_mu);
            auto it = peer->m_files.find(file_id);
            if (it == peer->m_files.end() || idx >= it->second.chunk_count) return;
            f = &it->second;
            if (f->chunks[idx].has_value()) return;   // 鍘婚噸锛堝潡宸叉敹鍒帮級
            f->chunks[idx] = std::move(chunk);
            ++f->received;
            if (f->received == f->chunk_count) complete = true;
        }
        if (complete && f) {
            // 閲嶇粍鏂囦欢锛堥攣澶栨嫾鎺ワ紝閬垮厤闀胯€楁椂鎸侀攣锟?
            Bytes out;
            out.reserve(f->total);
            for (auto& c : f->chunks) {
                if (c) out.insert(out.end(), c->begin(), c->end());
            }
            std::string name = f->name;
            std::lock_guard<std::mutex> lk(peer->m_mu);
            peer->m_files.erase(file_id);
            fire_file(peer, name, std::move(out));
        }
    }

    // 鎺ユ敹绔秷鎭垎鍙戯細璇嗗埆 file/data 鍐呴儴娑堟伅锛屽叾浣欒蛋搴旂敤 message_callback
    void deliver_message(Peer* peer, Bytes payload) {
        if (!payload.empty()) {
            std::uint8_t tag = payload[0];
            if (tag == 0x01) { handle_file_manifest(peer, payload, 1); return; }
            if (tag == 0x02) { handle_file_chunk(peer, payload, 1); return; }
            if (tag == 0x03) {
                fire_data_msg(peer, Bytes(payload.begin() + 1, payload.end()));
                return;
            }
        }
        TightTransport::MessageCallback cb_copy;
        {
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            cb_copy = m_message_cb;
        }
        if (!cb_copy) return;
        cb_copy(peer->m_id, std::move(payload));
    }

    void fire_command(Peer* peer, Bytes payload) {
        TightTransport::CommandCallback cb_copy;
        {
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            cb_copy = m_command_cb;
        }
        if (!cb_copy) return;
        cb_copy(peer->m_id, std::move(payload));
    }

    void fire_peer_event(Peer* peer, LinkState new_state) {
        TightTransport::PeerCallback cb_copy;
        {
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            cb_copy = m_peer_cb;
        }
        if (!cb_copy) return;
        PeerEvent ev{};
        ev.id = peer->m_id;
        ev.role = peer->m_role;
        ev.state = new_state;
        ev.client_id = peer->m_peer_client_id;
        cb_copy(ev);
    }

    bool start() {
        if (m_running.load()) return true;
        if (!wsa_acquire()) return false;

        if (!m_ext_socket) {
        m_sock = static_cast<NativeSocket>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (m_sock == kInvalidSocket) {
            wsa_release();
            return false;
        }
        // L4S锛圧FC 9331锛夛細鍑虹珯鎶ユ枃锟?ECT(1)锛屼緵鏀寔 ECN 鐨勭綉缁滄爣锟?CE锟?
        tight_detail::ecn::set_ect1(m_sock);

        sockaddr_in local{};
        if (!resolve_address(m_config.bind.host, m_config.bind.port, local)) {
            close_socket(m_sock);
            m_sock = kInvalidSocket;
            wsa_release();
            return false;
        }
        if (::bind(m_sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            close_socket(m_sock);
            m_sock = kInvalidSocket;
            wsa_release();
            return false;
        }

        // Large buffers are essential on localhost: the receiver thread does
        // per-packet work (dispatch + logging), so a small kernel buffer
        // (Windows default 64 KiB ~ 54 x 1200-byte datagrams) overflows and
        // silently drops datagrams under any burst. 8 MiB absorbs bursts.
        // lite_mode 鑷姩鏀剁揣锟?16 KiB锛堝鎴风鍗曡繛鎺ユ棤绐佸彂姹囪仛鍦烘櫙锛夛拷?
        std::size_t buf_bytes = m_config.socket_buffer_bytes;
        if (m_config.lite_mode) {
            // lite 内核缓冲收紧：Audio ≤4KB（音频流量极小）、Video ≤16KB
            buf_bytes = std::min<std::size_t>(
                buf_bytes, m_config.lite_profile == tight::LiteProfile::Audio ? 4 * 1024
                                                                              : 16 * 1024);
        }
        int bufsize = static_cast<int>(buf_bytes);
        tight_setsockopt(m_sock, SOL_SOCKET, SO_RCVBUF,
                         reinterpret_cast<const char*>(&bufsize), sizeof(bufsize));
        tight_setsockopt(m_sock, SOL_SOCKET, SO_SNDBUF,
                         reinterpret_cast<const char*>(&bufsize), sizeof(bufsize));
#ifdef _WIN32
        // On Windows, set SO_EXCLUSIVEADDRUSE off and disable WSAECONNRESET
        // delivery on UDP sockets (we don't want ICMP unreachable errors to
        // abort the recvfrom loop when a peer disappears mid-stream).
        BOOL false_ = FALSE;
        setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&false_), sizeof(false_));
        // SIO_UDP_CONNRESET (0x9800000C) -- disable WSAECONNRESET errors.
        DWORD bytes_returned = 0;
        WSAIoctl(m_sock, SIO_UDP_CONNRESET, &false_, sizeof(false_),
                  nullptr, 0, &bytes_returned, nullptr, nullptr);
        u_long nonblock = 1;
        ioctlsocket(m_sock, FIONBIO, &nonblock);
#else
        int fl = fcntl(m_sock, F_GETFL, 0);
        if (fl >= 0) fcntl(m_sock, F_SETFL, fl | O_NONBLOCK);
#endif

        sockaddr_in bound{};
        SockLen blen = sizeof(bound);
        if (::getsockname(m_sock, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
            m_local_port = ntohs(bound.sin_port);
        }
        } else {
            // 外部 socket 模式：不创建/绑定/配置 socket，本地端口仅作标识
            // （无真实绑定时为配置值），接收由 inject_packet() 注入。
            m_local_port = m_config.bind.port;
        }

        m_running.store(true);
        // 视频容量通知线程：独立线程调应用回调（变化迟滞后入队触发），
        // 接收线程不阻塞。lite/普通模式共用。
        m_cap_thread = SmallThread([this] { cap_notify_loop(); }, 0);
        // 绮剧畝妯″紡鍗曠嚎绋嬶細receiver 鑱岃矗鍚屾牱锟?reactor 鑺傛媿鍚堝苟锛坉rain_receiver锟?
        m_reactor_thread = SmallThread([this] { reactor_loop(); }, thread_stack());
        if (!m_lite_mode.load()) {
            // 完整模式 4 线程（外部 socket 模式无接收线程：收包由应用注入）
            m_workers_running.store(true);
            if (!m_ext_socket) {
                m_receiver_thread = SmallThread([this] { receiver_loop(); }, 0);
            }
            m_encode_thread = SmallThread([this] { encode_loop(); }, 0);
            m_sender_thread = SmallThread([this] { sender_loop(); }, 0);
        } else if (m_config.lite_profile == tight::LiteProfile::Video && !m_ext_socket) {
            // lite 视频双线程：receiver（recvfrom + 协议处理/解密/重组）独立
            // 线程——高码率视频负载不再被 reactor 节拍串行拖慢（单线程
            // 实测 UDP 丢失 drop 742）；encode/sender 职责仍在 reactor
            // 节拍（drain_encode/drain_sender）。Audio 保持单线程（最低
            // 内存/功耗）。
            m_workers_running.store(true);
            m_receiver_thread = SmallThread([this] { receiver_loop(); }, 0);
        }
        return true;
    }

    void stop() {
        if (!m_running.exchange(false)) {
            return;
        }
        m_workers_running.store(false);
        m_encode_queue.close();
        m_outbound_queue.close();
        m_cap_queue.close();
        if (m_cap_thread.joinable()) {
            try { m_cap_thread.join(); } catch (...) {}
        }
        if (m_reactor_thread.joinable()) {
            try { m_reactor_thread.join(); } catch (...) {}
        }
        if (m_receiver_thread.joinable()) {
            try { m_receiver_thread.join(); } catch (...) {}
        }
        if (m_encode_thread.joinable()) {
            try { m_encode_thread.join(); } catch (...) {}
        }
        if (m_sender_thread.joinable()) {
            try { m_sender_thread.join(); } catch (...) {}
        }
        if (m_sock != kInvalidSocket) {
            close_socket(m_sock);
            m_sock = kInvalidSocket;
        }
        wsa_release();
    }

    bool connect(const RemotePeer& remote) {
        if (!m_running.load()) return false;
        sockaddr_in addr{};
        if (!resolve_address(remote.address.host, remote.address.port, addr)) return false;
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        AddrKey key{addr.sin_addr.s_addr, addr.sin_port};
        auto addr_it = m_peer_by_addr.find(key);
        if (addr_it != m_peer_by_addr.end() && addr_it->second != remote.id) {
            // Remove the old peer entry; the new add_peer below will create a fresh one.
            // We can't move a Peer (it holds a mutex), so we just erase and recreate.
            std::string old_id = addr_it->second;
            m_peers.erase(old_id);
            addr_it->second = remote.id;
        }
        auto& peer = m_peers[remote.id];
        peer.m_id = remote.id;
        peer.m_addr = addr;
        peer.m_addr_set = true;
        peer.m_role = LinkRole::Node;
        peer.m_reconnect = true;
        peer.m_drop_log = m_config.drop_log && !m_lite_mode.load();
        peer.m_retransmit = m_config.retransmit_enabled;
        std::copy(std::begin(m_config.channel_reliable), std::end(m_config.channel_reliable),
                            peer.m_channel_reliable.begin());
        m_peer_by_addr[key] = remote.id;
        if (peer.m_state == LinkState::Closed) {
            peer.m_state = LinkState::Handshake;
            peer.m_last_handshake_sent = std::chrono::steady_clock::now() - std::chrono::hours(1);
            peer.m_handshake_backoff = std::chrono::milliseconds(500);
        }
        send_handshake(&peer);
        return true;
    }

    bool send_message(const std::string& peer_id, Bytes payload, int priority = 0,
                      std::uint8_t channel = 0, bool keyframe = false) {
        if (!m_running.load()) return false;
        // 鍗曟潯娑堟伅闀垮害涓婇檺锛堥粯锟?64KB锛屽彲閰嶇疆锟?10MB锟?
        if (payload.size() > max_message_bytes()) return false;
        {
            std::lock_guard<std::mutex> lock(m_send_mutex);
            // 瀹归噺妫€鏌ヨ鐩栨秷鎭骇绠＄嚎锛堝彂閫侀槦锟?+ 缂栫爜闃熷垪锛屾寜"娑堟伅"璁★級锟?
            // 鍙戦€侀槦鍒楁瘡鎷嶈 process_send_queue 娓呯┖锛屽彧锟?m_send_queue
            // 浼氳 send() 鍦ㄦ寔缁儗鍘嬩笅姘歌繙杩斿洖 true锛屾秷鎭殢鍚庡湪鍥炲璺緞
            // 琚潤榛樹涪寮冿紝杩濊儗"闃熷垪婊¤繑锟?false"锟?API 濂戠害锟?
            // 鍑虹珯闃熷垪婊℃椂 send_raw 浼氶潤榛樹涪寮冩暟鎹姤锛堜粎锟?NACK 閲嶄紶
            // 鍏滃簳锛屾氮璐归摼璺閲忥級锛屽悓鏍疯璁╁簲鐢ㄥ敖鏃╂劅鐭ヨ儗鍘嬶拷?
            std::size_t total = 0;
            for (const auto& kv : m_send_queue) total += kv.second.size();
            total += m_encode_queue.size();
            if (total >= queue_limit()) {
                return false;
            }
            if (m_outbound_queue.capacity() > 0 &&
                m_outbound_queue.size() >= m_outbound_queue.capacity()) {
                return false;
            }
            m_send_queue[priority].emplace_back(
                SendMsg{peer_id, std::move(payload), channel, keyframe});
            // 关键帧提交标记：fast 排空窗口内应用重启编码器后的新 IDR 到达
            // 即视为"IDR 已发出"（入队到实际发送 ≤100ms < 报告周期 333ms，
            // 下一报告到达时必然已发出）——fast 窗口结束条件之一。
            // 同时延长冻结期（关键帧超发并入 fast 冻结）：IDR 60KB 在低带
            // 宽链路传输 27-33ms 触达 l4s 阈值 → proxy CE——突刺 CE 是帧
            // 自身传输，不算拥塞——冻结期内无视 CE（延长到 IDR 提交后
            // 800ms：传输 ~30ms + 报告确认 333ms + 余量）。
            if (keyframe && m_evac_mode.load() == 1) {
                m_evac_keyframe_sent.store(true);
                auto hold_until = (std::chrono::steady_clock::now() + kKeyframeHoldTotal)
                                      .time_since_epoch().count();
                auto cur = m_evac_freeze_until_ns.load();
                if (cur == 0 || hold_until > static_cast<std::int64_t>(cur)) {
                    m_evac_freeze_until_ns.store(hold_until);
                }
            }
            m_last_app_send_ms.store(unix_millis());  // 搴旂敤鏁版嵁娲诲姩鏃堕棿鎴筹紙app_limited 鍒ゅ畾锟?
        }
        return true;
    }

    bool send_command(const std::string& peer_id, Bytes payload) {
        if (!m_running.load()) return false;
        // Commands fit in a single datagram: no fragmentation/reassembly.
        std::size_t max_payload = m_config.mtu > kHeaderSize ? m_config.mtu - kHeaderSize : 0;
        if (payload.size() > max_payload) return false;
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto pit = m_peers.find(peer_id);
        if (pit == m_peers.end()) {
            pit = std::find_if(m_peers.begin(), m_peers.end(), [&](const auto& entry) {
                return entry.second.m_id == peer_id;
            });
        }
        if (pit == m_peers.end()) return false;
        auto& peer = pit->second;
        // Established 鍗冲厑璁稿彂閫侊細浼氳瘽瀵嗛挜锟?Established 鏃跺弻鏂瑰凡灏辩华
        // 锛堟敹鍒板锟?Handshake/HandshakeAck 鍗虫淳鐢燂級锛孫nline 閫氬憡鐨勫崟锟?
        // 涓㈠け涓嶅簲闃诲鍛戒护閫氶亾锟?
        if (peer.m_state != LinkState::Online && peer.m_state != LinkState::Established) return false;
        PacketHeader header{};
        header.magic = kMagic;
        header.version = kVersion;
        header.type = PacketType::Command;
        header.client_id = m_local_client_id;
        header.session_id = m_local_session_id;
        {
            std::lock_guard<std::mutex> plock(peer.m_mu);
            header.sequence = peer.m_cmd_seq_out++;
        }
        header.payload_size = static_cast<std::uint16_t>(payload.size());
        header.tick = static_cast<std::uint32_t>(unix_millis() & 0xFFFFFFFFULL);
        // 鍛戒护鐩村彂锛堜笉璧伴槦鍒楋級锛氬懡锟?鎺у埗閫氶亾涓嶈兘琚暟鎹椽娉涙尋鍗狅紝
        // 鍚﹀垯鍙嶅悜绉帇鏃跺懡浠よ繜鍒版暟绉掞紙锟?send_direct 娉ㄩ噴锛夛拷?
        send_direct(&peer, build_wire_packet(&peer, header, payload));
        return true;
    }

    std::vector<PeerEvent> peers_snapshot() const {
        std::vector<PeerEvent> out;
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        out.reserve(m_peers.size());
        for (const auto& kv : m_peers) {
            PeerEvent ev{};
            ev.id = kv.second.m_id;
            ev.role = kv.second.m_role;
            ev.state = kv.second.m_state;
            ev.client_id = kv.second.m_peer_client_id;
            out.push_back(std::move(ev));
        }
        return out;
    }

    void reactor_loop() {
        auto next_tick = std::chrono::steady_clock::now();
        while (m_running.load(std::memory_order_acquire)) {
            send_handshakes();
            send_heartbeats();
            send_online_announcements();
            send_reports();
            check_report_timeouts();
            check_offline();
            flush_commands();
            process_send_queue();
            if (m_lite_mode.load(std::memory_order_acquire)) {
                // 单线程（Audio）合并 receiver 职责；视频双线程时接收已由
                // 独立 receiver_thread 承担（recvfrom 不在此串行）。
                // 外部 socket 模式无内部 socket 可读，收包由 inject_packet 注入。
                if (m_config.lite_profile != tight::LiteProfile::Video && !m_ext_socket) {
                    drain_receiver();
                }
                drain_encode();   // 鍚堝苟 encode 绾跨▼鑱岃矗
                drain_sender();   // 鍚堝苟 sender 绾跨▼鑱岃矗
            }
            next_tick += flush_interval();
            auto now = std::chrono::steady_clock::now();
            if (now < next_tick) {
                std::this_thread::sleep_for(next_tick - now);
            } else {
                std::this_thread::yield();
            }
        }
    }

    // 绮剧畝妯″紡锛歳eactor 鑺傛媿鍐呴『甯︽敹鍖咃紙鏇夸唬鐙珛 receiver 绾跨▼锛夛拷?
    // socket 涓洪潪闃诲锛屾瘡鎷嶆渶澶氬锟?64 涓姤鏂囷紝淇濊瘉 reactor 涓嶈楗挎锟?
    void drain_receiver() {
        std::uint8_t buf[2048];
        for (int i = 0; i < 64; ++i) {
            sockaddr_in from{};
            SockLen flen = sizeof(from);
            int n = tight_recvfrom(m_sock, reinterpret_cast<char*>(buf),
                                   static_cast<int>(sizeof(buf)), 0,
                                   reinterpret_cast<sockaddr*>(&from), &flen);
            if (n < 0) return;
            // L4S CE 标记数据报：仅计数（ce_ratio 独立上报）。
            // CE 驱动拥塞判定（降速），但**不**计入迟到率——否则 FEC 冗余
            // 随 CE 启用（L4S 无丢包时冗余纯浪费）→ 线上超发 → 更多 CE →
            // 恶性循环（实测 btl 连崩到底）。FEC 冗余只由"丢失+超线"驱动。
            if (tight_detail::ecn::is_ce_mark(buf, n)) {
                Peer* peer = find_or_create_peer_by_addr(from);
                std::lock_guard<std::mutex> lk(peer->m_mu);
                peer->m_ce_marks += 1;
                continue;
            }
            if (static_cast<std::size_t>(n) < kHeaderSize) continue;
            PacketHeader header{};
            Bytes payload;
            // 鐩存帴浠庢爤缂撳啿瑙ｇ爜锛堟祦锟?CRC锛夛紝鍏嶅幓 datagram 鎷疯礉
            if (!PacketCodec::decode(buf, static_cast<std::size_t>(n), header, payload)) continue;
            handle_packet(from, header, payload);
        }
    }

    // 绮剧畝妯″紡锛歳eactor 鑺傛媿鍐呴『甯︽秷璐圭紪鐮侀槦鍒楋紙鏇夸唬鐙珛 encode 绾跨▼锟?
    void drain_encode() {
        for (int i = 0; i < 16; ++i) {
            auto task = m_encode_queue.poll();
            if (!task) return;
            // 通道排空：该通道编码任务出队即丢（同 encode_loop）
            auto now_ms = unix_millis();
            if (task->m_channel < 8 && now_ms < m_drain_until_ms[task->m_channel].load()) {
                continue;
            }
            try { fragment_and_send(task->m_peer, std::move(task->m_payload), task->m_channel, task->m_keyframe); } catch (...) {}
        }
    }

    // 搴旂敤鍙楅檺妫€娴嬶紙BBR app-limited锛夛細鍑虹珯闃熷垪鍑犱箮涓虹┖涓旀渶锟?500ms 锟?
    // 搴旂敤鏃犳暟鎹彂閫佹椂鎵嶈涓虹湡搴旂敤鍙楅檺鈥斺€旀鏃舵姇閫掔巼鏍锋湰 = 搴旂敤閫熺巼锛屼笉
    // 浠ｈ〃閾捐矾瀹归噺锛堜笉椹卞姩 btl 璺熻穼锛夈€備粎 outbound 绌轰絾搴旂敤鎸佺画鐏屾暟锟?
    // 锛堝瑙嗛 30fps锛宲acer 楂樹簬搴旂敤+閾捐矾銆佸彂閫佸嵆璧帮級鏃朵笉锟?app-limited锟?
    // 鎶曢€掔巼鍙嶆槧鐪熷疄閾捐矾瀹归噺锛屽繀椤诲弬锟?btl 璺熻穼锛屽惁锟?btl 鍗″湪鍒濆鍊硷拷?
    // 鍙戦€佹椽姘存拺鐖嗕笅娓搁槦鍒楋紙瀹炴祴寤惰繜椋欏埌 2s+ 鍗℃锛夛拷?
    bool app_limited() const {
        // 出站队列 ≤8 包视为"近空"（应用速率即上限）：音视频混合流下
        // 音频 50fps×2 包让队列常态 >1，若用 >1 判定 app_limited 永不
        // 生效 → 投递率样本（=应用速率）把 btl 拖到应用速率 → 排空片
        // 令牌 0.75×btl < 应用速率 → 持续积压排空循环（follow 实测）。
        // 8 包 ≈ 10KB ≈ 80ms 积压：真实拥塞时跟跌最多延迟 1 个报告。
        if (m_outbound_queue.size() > 8) return false;
        auto now = unix_millis();
        return now - m_last_app_send_ms.load() > 500;
    }

    // 闄愰€熺粦瀹氭爣蹇楋細浠ょ墝妗跺洜鐪熷疄绉帇锛堚墺32 涓暟鎹姤锛夎€屽崱浣忚繃
    // 锛坧acer-limited锛夈€備粎鐢ㄤ簬鎺掔┖鐗囬棬鎺э紙鎺㈡祴鐗囩湡鐨勫缓杩囬槦鍒楁墠鎺掔┖锛夛紱
    // 涓嶇敤浜庢嫆缁濇姇閫掔巼鏍锋湰鈥斺€旀嫆缁濅細璁╁甫瀹戒笅闄嶆椂浼拌鏃犳硶璺熻繘锛堝疄锟?
    // step-down 锟?btl 鍗″湪鏃э拷?18s+锛夈€傜灛鏃剁獊鍙戝 5400B 妗剁殑鐭殏
    // 鑰楀敖锛坋ncode 姣忔媿锟?16 鍖咃級涓嶄唬锟?pace 鍙楅檺锛岃鏍囦細璁╂帓绌虹墖
    // 鍦ㄥ共鍑€閾捐矾涓婅鍚€佸嚭鐜板懆鏈燂拷?RTT 閿娇锟?
    std::atomic<bool> m_pacer_limited{false};

    // 绮剧畝妯″紡锛歳eactor 鑺傛媿鍐呴『甯︽秷璐瑰嚭绔欓槦鍒楋紙鏇夸唬鐙珛 sender 绾跨▼锛夛拷?
    // 浠ょ墝涓嶈冻鏃朵繚鐣欏綋鍓嶆姤鏂囧埌涓嬩竴鎷嶏紝涓嶉樆锟?reactor锟?
    void drain_sender() {
        // 音频队列：绕过令牌，一次性清空（实时音频无条件优先，同 sender_loop）
        for (;;) {
            auto pkt = m_audio_queue.poll();
            if (!pkt) break;
            auto now_ms = unix_millis();
            if (pkt->m_channel < 8 && now_ms < m_drain_until_ms[pkt->m_channel].load()) continue;
            if (!pkt->m_peer->m_addr_set || !socket_ready()) continue;
            m_tx_bytes.fetch_add(pkt->m_datagram.size());
            socket_send(pkt->m_peer->m_addr, pkt->m_datagram.data(),
                        pkt->m_datagram.size());
        }
        for (int i = 0; i < 64; ++i) {
            if (!m_lite_pending) {
                auto pkt = m_outbound_queue.poll();
                if (!pkt) return;
                // 通道排空：出队即丢（同 sender_loop）
                auto now_ms = unix_millis();
                if (pkt->m_channel < 8 && now_ms < m_drain_until_ms[pkt->m_channel].load()) {
                    continue;
                }
                m_lite_pending = std::move(*pkt);
            }
            auto& pkt = *m_lite_pending;
            if (!pkt.m_peer->m_addr_set || !socket_ready()) {
                m_lite_pending.reset();
                continue;
            }
            double cost = static_cast<double>(pkt.m_datagram.size());
            refill_token_bucket();
            if (m_token_bucket < cost && !app_limited()) {
                if (m_outbound_queue.size() >= 32) m_pacer_limited.store(true);
                return;   // 閾捐矾鎷ュ涓斾护鐗屼笉瓒筹細涓嬩竴鎷嶅啀锟?
            }
            m_token_bucket -= cost;
            m_tx_bytes.fetch_add(pkt.m_datagram.size());
            socket_send(pkt.m_peer->m_addr, pkt.m_datagram.data(),
                        pkt.m_datagram.size());
            m_lite_pending.reset();
        }
    }

    // 视频容量通知线程：取队列值调应用回调（回调须快速返回，只做存储/
    // 编码器调整）。独立线程保证接收线程不阻塞。
    void cap_notify_loop() {
        while (m_running.load(std::memory_order_acquire)) {
            auto opt = m_cap_queue.take();
            if (!opt) break;
            TightTransport::VideoCapacityCallback cb;
            {
                std::lock_guard<std::mutex> lock(m_callback_mutex);
                cb = m_video_capacity_cb;
            }
            if (cb) cb(*opt);
        }
    }

    // file/data 通道（2/3）实时发送速率（bytes/s）：采样推进窗口，
    // video_capacity_bps 计算时扣除。调用频率即采样频率（报告周期/应用轮询）。
    double file_data_rate_bps() {
        std::lock_guard<std::mutex> lock(m_cap_mu);
        auto now = std::chrono::steady_clock::now();
        auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
            now - m_fd_rate_last_ts).count();
        std::uint64_t cur = m_fd_tx_bytes.load();
        double rate = 0.0;
        if (dt > 0 && m_fd_rate_last_ts.time_since_epoch().count() != 0) {
            rate = static_cast<double>(cur - m_fd_rate_last_bytes) * 1000000.0 / dt;
        }
        m_fd_rate_last_bytes = cur;
        m_fd_rate_last_ts = now;
        return rate;
    }

    // 实际 FEC 冗余率（校验片/数据片，滑动窗口 1s，全部 peer 累计比）。
    double fec_redundancy_ratio() {
        std::uint64_t data = 0, parity = 0;
        {
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            for (auto& kv : m_peers) {
                data += kv.second.m_fec_data_pkts.load();
                parity += kv.second.m_fec_parity_pkts.load();
            }
        }
        if (data == 0) return 0.0;
        return static_cast<double>(parity) / static_cast<double>(data);
    }

    // 视频可用码率（bps）：总带宽 − 音频固定预留 − file/data 实时速率，
    // 再按实际 FEC 冗余折算（线上数据容量 = 编码码率 × (1+冗余)）。
    // 音频预留 = audio_reserved_bps（音频编码码率）× (1 + channel_fec_extra[1])：
    // 校验片开销由应用是否设置 channel_fec_extra[1] 决定，不设置（0）即
    // 不预留校验，默认 0。应用据此直接设编码码率，无需再自行折让。
    // 清空出站积压（丢帧止损，仅视频，保留音频）：排空窗口快排使用——
    // 丢弃全部待发视频帧（send/encode/outbound 三层），音频保留（清空
    // 音频会让播放端 300ms 抖动缓冲被抽干 → 欠载 + 停顿）。
    //  - m_send_queue：清空 priority < 1（视频）；priority ≥ 1（音频等）保留
    //  - m_encode_queue：poll 全量，channel == 1 的编码任务回推保留
    //  - m_outbound_queue：已编码数据报无法区分通道；音频包驻留极短
    //    （20ms 才 2 包、发送周期 ≤10ms），排空损失 ≤1-2 个音频包，
    //    300ms 缓冲可吸收，可接受
    void clear_outbound_impl() {
        {
            std::lock_guard<std::mutex> lock(m_send_mutex);
            for (auto& kv : m_send_queue) {
                if (kv.first < 1) kv.second.clear();
            }
        }
        std::vector<EncodeTask> keep_audio;
        while (auto task = m_encode_queue.poll()) {
            if (task->m_channel == 1) keep_audio.push_back(std::move(*task));
        }
        for (auto& t : keep_audio) {
            m_encode_queue.try_push(std::move(t));
        }
        while (m_outbound_queue.poll()) {}
    }

    // 拥塞排空窗口（双模式，slowdown_window_ms > 0 时）：btl 量化大降
    // 后按降幅分流排空策略：
    //   fast（降幅 >50%：×0.45/×0.30/×0.20）——清空视频积压（clear_outbound，
    //     音频保留）+ 通道排空 100ms + 回调应用重启编码器出新 IDR，播放端
    //     跳到新 IDR 时间线，积压瞬间归零。窗口事件驱动：新 IDR 提交发送
    //     + 下一报告到达 → 结束；kEvacTimeout 兜底。
    //   slow（降幅 20%~50%：×0.65）——Q 面积法排空：快照超发积压 Q 与
    //     btl，窗口内 cap = (btl_snap×8 − 音频 − fd − Q×8/窗口)/(1+冗余)，
    //     3s 排完积压后结束（Q 清零）。
    // 回调与轮询共用本函数，应用侧零改动。
    std::uint64_t video_capacity_bps() {
        std::uint64_t btl = m_bandwidth.btl_bw_bps();
        double ratio = fec_redundancy_ratio();
        double fd_bps = file_data_rate_bps() * 8.0;
        double audio = static_cast<double>(m_config.audio_reserved_bps);
        audio *= 1.0 + static_cast<double>(m_config.channel_fec_extra[1]);  // 校验片按设置叠加
        double total_bps = static_cast<double>(btl) * 8.0;
        double cap = (total_bps - audio - fd_bps) / (1.0 + ratio);
        if (cap < 0.0) cap = 0.0;
        if (m_config.slowdown_window_ms > 0) {
            auto now = std::chrono::steady_clock::now();
            auto window = std::chrono::milliseconds(m_config.slowdown_window_ms);
            auto last_cong = m_bandwidth.last_congest_at();
            // 边沿（新一次剧烈降速，尚未处理）：按下降因子分流排空策略
            if (last_cong.time_since_epoch().count() > 0 &&
                last_cong != m_evac_processed_cong &&
                now - last_cong < window) {
                double factor = m_bandwidth.last_congest_factor();
                if (factor <= kEvacFastMaxFactor) {
                    // 降幅 ≥35%（柔表 ×0.50/×0.65 档，strength≥20% 重/严重
                    // 拥塞）：快速排空——清队列 + 新 IDR（破产清算：债务
                    // 清零由 sender_loop 的 m_evac_reset_token 处理）
                    clear_outbound_impl();
                    m_drain_until_ms[0].store(unix_millis() + kEvacDrainMs);
                    m_evac_reset_token.store(true);
                    m_evac_keyframe_sent.store(false);
                    m_evac_freeze_until_ns.store(
                        (now + kEvacFreezeTotal).time_since_epoch().count());  // 冻结期 = 排空 + 锁定
                    m_evac_mode.store(1);
                    notify_evac_keyframe();
                } else {
                    // 降幅 20%~50%（×0.65）：3 秒排空（Q 面积法）
                    std::lock_guard<std::mutex> lk(m_cap_mu);
                    m_evac_slow_q_bytes = m_evac_pending;
                    m_evac_slow_btl_snap = btl;
                    m_evac_slow_deadline = now + window;
                    m_evac_mode.store(2);
                }
                m_evac_processed_cong = last_cong;
            }
            int mode = m_evac_mode.load();
            if (mode == 1) {
                // fast：冻结期内 cap 按当前 btl 折算（积压已清，正常码率即可
                // ——IDR 与后续新帧低量发送）
                cap = (total_bps - audio - fd_bps) / (1.0 + ratio);
                if (cap < 0.0) cap = 0.0;
                // 冻结到期（触发+3s = 排空+锁定）：解冻，恢复判定自然开始
                auto freeze_ns = m_evac_freeze_until_ns.load();
                if (freeze_ns != 0 &&
                    now.time_since_epoch().count() >= static_cast<std::int64_t>(freeze_ns)) {
                    m_evac_freeze_until_ns.store(0);
                    if (!m_evac_keyframe_sent.load()) {
                        // 兜底：应用未在时限内出 IDR → 强制结束排空窗口
                        // （播放端 req-keyframe 兜底）
                        m_evac_mode.store(0);
                    }
                }
            } else if (mode == 2) {
                // slow：Q 面积法排空输出
                double window_s = static_cast<double>(m_config.slowdown_window_ms) / 1000.0;
                double evac_bps = static_cast<double>(m_evac_slow_q_bytes) * 8.0 / window_s;
                double total_snap = static_cast<double>(m_evac_slow_btl_snap) * 8.0;
                double cap_drain = (total_snap - audio - fd_bps - evac_bps) / (1.0 + ratio);
                if (m_evac_slow_q_bytes == 0) cap_drain = cap * 0.5;  // Q 不可得时减半兜底
                if (cap_drain < 0.0) cap_drain = 0.0;
                cap = cap_drain;
                if (now >= m_evac_slow_deadline) {
                    // 3s 到期：排空完成，结束窗口（Q 清零，下次重新累计）
                    m_evac_mode.store(0);
                    m_evac_slow_q_bytes = 0;
                    m_evac_slow_btl_snap = 0;
                    m_evac_slow_deadline = {};
                    std::lock_guard<std::mutex> lk(m_cap_mu);
                    m_evac_pending = 0;
                }
            }
        }
        return static_cast<std::uint64_t>(cap);
    }

    // 视频容量迟滞通知：相对变化 >10% 且绝对 >100k 才入队（防频繁回调）。
    // 接收线程调用（btl/冗余率更新后），m_last_cap_notified 受 m_cap_mu
    // 保护（video_capacity_bps 轮询路径也读取它做排空窗口 Q 快照）。
    void notify_video_capacity() {
        std::uint64_t cap = video_capacity_bps();
        std::uint64_t last = 0;
        {
            std::lock_guard<std::mutex> lk(m_cap_mu);
            last = m_last_cap_notified;
        }
        if (last == 0) {
            std::lock_guard<std::mutex> lk(m_cap_mu);
            m_last_cap_notified = cap;
            m_cap_queue.try_push(cap);
            return;
        }
        std::uint64_t delta = cap > last ? cap - last : last - cap;
        bool rel = delta * 10 > last;      // 相对 >10%
        bool abs = delta > 100000;         // 绝对 >100k
        if (rel && abs) {
            std::lock_guard<std::mutex> lk(m_cap_mu);
            m_last_cap_notified = cap;
            m_cap_queue.try_push(cap);
        }
    }

    // 浠ょ墝妗剁獊鍙戜笂闄愶細鑷冲皯瀹圭撼涓€涓妭鎷嶇殑瀹為檯鏃堕暱锛圵indows 鐫＄湢绮掑害
    // ~15.6ms銆丷TOS 鎶栧姩锛夌殑鍙戦€侀搴︼紝閬垮厤鑺傛媿锟?4脳mtu 鐨勫浐瀹氫笂锟?
    // 閽冲埗锟?0ms 鑺傛媿 脳 5400B 锟?407KB/s < 4Mbps 閾捐矾瀹归噺锛宐tl 姘歌繙
    // 杩戒笉涓婂疄闄呭甫瀹斤級銆備笅闄愪繚锟?mtu脳4 鐨勫師绐佸彂璇箟锟?
    double token_bucket_cap() const {
        return std::max(static_cast<double>(m_config.mtu) * 4.0,
                        static_cast<double>(m_bandwidth.bytes_per_second()) * 0.02);
    }

    // 令牌贷款额度（字节）：btl × loan_seconds（默认 1s，动态随 btl 收窄——
    // 弱网超发危害越大，可贷额度越小）。贷款用于视频帧随心跳一次性发完
    // 及覆盖编码联动延迟（1-2s）；贷款耗尽 → 排空视频 + 回调（硬止损）。
    double loan_limit() const {
        if (m_config.loan_seconds <= 0.0) return 0.0;
        return static_cast<double>(m_bandwidth.bytes_per_second()) * m_config.loan_seconds;
    }

    // 贷款耗尽/恢复回调（在 sender 线程调用，须快速返回）：exhausted=true
    // 视频被持续排空（债务清零前不发）；false 债务清零、发送恢复。
    void notify_loan_exhausted(bool exhausted) {
        TightTransport::LoanExhaustedCallback cb;
        {
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            cb = m_loan_exhausted_cb;
        }
        if (cb) cb(exhausted);
    }

    // 拥塞排空窗口回调（接收/轮询线程调用，须快速返回）：应用重启编码器
    // （force_keyframe → 新 IDR + 低码率），播放端跳到新 IDR 时间线。
    void notify_evac_keyframe() {
        TightTransport::EvacKeyframeCallback cb;
        {
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            cb = m_evac_keyframe_cb;
        }
        if (cb) cb();
    }

    void refill_token_bucket() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now - m_token_bucket_time).count();
        if (elapsed_us <= 0) return;
        std::uint64_t bps = m_bandwidth.bytes_per_second();
        double tokens = static_cast<double>(bps) * static_cast<double>(elapsed_us) / 1000000.0;
        m_token_bucket += tokens;
        double cap = token_bucket_cap();
        if (m_token_bucket > cap) m_token_bucket = cap;
        m_token_bucket_time = now;
    }

    Peer* find_or_create_peer_by_addr(const sockaddr_in& from) {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        AddrKey key{from.sin_addr.s_addr, from.sin_port};
        auto it = m_peer_by_addr.find(key);
        if (it != m_peer_by_addr.end()) {
            auto pit = m_peers.find(it->second);
            if (pit != m_peers.end()) return &pit->second;
        }
        std::string id = "anon-" + std::to_string(static_cast<unsigned>(ntohs(from.sin_port))) +
                         "-" + std::to_string(random_u64() & 0xFFFFFFFFu);
        auto& peer = m_peers[id];
        peer.m_id = id;
        peer.m_addr = from;
        peer.m_addr_set = true;
        peer.m_role = LinkRole::Leaf;
        peer.m_drop_log = m_config.drop_log && !m_lite_mode.load();
        peer.m_retransmit = m_config.retransmit_enabled;
        // FEC 总开关（fec_enabled=false 或 lite 模式默认关闭）：
        // 发送端 fragmenter 据此不生成校验片（parity_count=0）
        peer.m_fec_disable.store(!m_config.fec_enabled);
        std::copy(std::begin(m_config.channel_reliable), std::end(m_config.channel_reliable),
                            peer.m_channel_reliable.begin());
        peer.m_state = LinkState::Handshake;
        peer.m_last_handshake_sent = std::chrono::steady_clock::now() - std::chrono::hours(1);
        m_peer_by_addr[key] = id;
        return &peer;
    }

    // header/payload 鎸夊€间紶閫掞細瑙ｅ瘑璺緞闇€瑕佸氨鍦拌繕鍘熸槑锟?
    void handle_packet(const sockaddr_in& from, PacketHeader header, Bytes payload) {
        // NOTE: no per-packet logging here. This fires for every datagram
        // (data/parity/ack/heartbeat) on the single receiver thread; a log
        // write + fflush per datagram slows the recvfrom loop enough to
        // overflow the kernel UDP buffer and cause massive packet loss.
        Peer* peer = find_or_create_peer_by_addr(from);

        peer->m_last_recv = std::chrono::steady_clock::now();
        if (peer->m_peer_client_id == 0 && header.client_id != 0) {
            peer->m_peer_client_id = header.client_id;
        }
        if (peer->m_peer_session_id == 0 && header.session_id != 0) {
            peer->m_peer_session_id = header.session_id;
        }
        // 瀹為檯鎺ユ敹瀛楄妭鏁帮紙锟?GCM 鏍囩锛夛細锟?Report 涓婃姤涓烘姇閫掔巼鏍锋湰锟?
        // 鐢遍摼璺摱棰堝喅瀹氾紝涓嶅彈 ACK 娓告爣璺崇己褰卞搷銆傛暟锟?鏍￠獙鍖呰鏁扮敤锟?
        // 璁＄畻 CE 鍗犳瘮锟?
        {
            std::lock_guard<std::mutex> lock(peer->m_mu);
            peer->m_recv_bytes += kHeaderSize + header.payload_size;
            if (header.type == PacketType::Data || header.type == PacketType::Parity) {
                peer->m_data_pkts += 1;
            }
        }

        // AES-256-GCM锛氬厛瑙ｅ瘑璐熻浇鍐嶆寜绫诲瀷鍒嗗彂锛堟姤鏂囧ご濮嬬粓涓烘槑鏂囷級
        if (header.flags & kFlagEncrypted) {
            if (!decrypt_payload(peer, header, payload)) {
                return;  // 鏈崗鍟嗗瘑閽ユ垨璁よ瘉澶辫触锛氫涪锟?
            }
        }

        bool need_ack = false;
        std::uint32_t ack_to_send = 0;

        switch (header.type) {
            case PacketType::Handshake:    handle_handshake(peer, header, payload);
                                           need_ack = true; ack_to_send = header.sequence; break;
            case PacketType::HandshakeAck: handle_handshake_ack(peer, header, payload);
                                           need_ack = true; ack_to_send = header.sequence; break;
            case PacketType::Online:       handle_online(peer, header, payload);
                                           need_ack = true; ack_to_send = header.sequence; break;
            case PacketType::Heartbeat:    handle_heartbeat(peer, header, payload); break;
            case PacketType::Bye:          handle_bye(peer, header, payload); break;
            case PacketType::Data:         handle_data(peer, header, payload); break;
            case PacketType::Parity:
                // FEC 关闭（lite 模式）时跳过校验片：不解码、不参与重组
                // （对端同样不生成——两端同配置；残留校验片直接忽略）
                if (m_config.fec_enabled) handle_data(peer, header, payload);
                break;
            case PacketType::Ack:          handle_ack(peer, header); break;
            case PacketType::Report:       handle_report(peer, header, payload); break;
            case PacketType::Probe:        handle_probe(peer, header); break;
            case PacketType::Command:      handle_command(peer, header, payload); break;
        }

        if (need_ack) {
            send_ack_packet(peer, ack_to_send);
        }
    }

    void handle_handshake(Peer* peer, const PacketHeader& header, const Bytes& payload) {
        if (payload.size() < 3) return;
        std::uint8_t role_byte = payload[0];
        std::uint16_t id_size = static_cast<std::uint16_t>(payload[1]) << 8U;
        id_size |= static_cast<std::uint16_t>(payload[2]);
        if (payload.size() < 3U + id_size) return;
        std::string peer_id(reinterpret_cast<const char*>(payload.data() + 3), id_size);
        // 璐熻浇灏鹃儴甯冨眬锛歔token][pubkey 32B?][flags 1B]
        // flags bit0 = 瀵圭 retransmit_enabled 閫氬憡锛堣璇佹椂涓嬪彂/涓婃姤锟?
        // 浠讳竴绔彲鍗曟柟闈㈠叧闂湰閾捐矾鐨勯噸浼犵紦鍐诧級锟?
        std::size_t token_end = payload.size();
        if (token_end > 3U + id_size) {
            peer->m_peer_retransmit = (payload[token_end - 1] & 0x01U) != 0;
            token_end -= 1;
        }
        std::array<std::uint8_t, 32> peer_pub{};
        bool have_pub = false;
        if (m_config.encryption_enabled && token_end >= 3U + id_size + 32) {
            token_end -= 32;
            std::memcpy(peer_pub.data(), payload.data() + token_end, 32);
            have_pub = true;
        }
        std::string token(reinterpret_cast<const char*>(payload.data() + 3 + id_size),
                          token_end - 3U - id_size);
        if (token != m_config.token) return;
        // Clock sync (瀵硅〃) at handshake time; the command channel also
        // restarts on a fresh session.
        sync_clock(peer, header.tick, unix_millis());
        CommandChannel::reset(*peer);
        // 新会话：重置报告接收时间戳（旧会话的停滞不应在重连握手期
        // 误触发 check_report_timeouts 的 btl 降速）
        peer->m_last_report_at = {};
        // 锟?id 淇濈暀鎺ュ叆鏃跺垎閰嶇殑鍖垮悕韬唤锛坅non-*锛夛紝淇濊瘉 token 鏍￠獙锟?
        // ECDH 鍏挜澶勭悊鐓у父杩涜锛堝惁鍒欎袱绔姞瀵嗙姸鎬佷笉瀵圭О锛屽瘑鏂囪鍗曞悜涓㈠純锟?
        if (id_size > 0) {
            peer->m_id = std::move(peer_id);
        }
        peer->m_role = role_byte == static_cast<std::uint8_t>(LinkRole::Node)
                         ? LinkRole::Node
                         : LinkRole::Leaf;
        // ECDH锛氱敤瀵圭鍏挜娲剧敓 AES-256-GCM 浼氳瘽瀵嗛挜
        if (have_pub) derive_session_key(peer, peer_pub, header.client_id);
        if (!peer->m_reconnect && peer->m_addr_set) {
            send_handshake(peer);
        }
        if (peer->m_state != LinkState::Established && peer->m_state != LinkState::Online) {
            peer->m_state = LinkState::Established;
            fire_peer_event(peer, LinkState::Established);
        }
        // HandshakeAck 璐熻浇灏鹃儴甯冨眬锟?Handshake 涓€鑷达細[pubkey 32B?][flags 1B]锟?
        // 鍙屾柟鍚屾椂 connect() 鏃讹紝瀵圭鍙兘鍙敹鍒版湰 Ack 鑰屾敹涓嶅埌鎴戜滑锟?
        // Handshake锛堜緥濡傚畠寮€濮嬬洃鍚墠鎴戜滑锟?Handshake 宸蹭涪澶憋級锛屾惡甯﹀叕锟?
        // 涓庤兘鍔涙爣蹇楀彲璁╁绔粎锟?Ack 瀹屾垚 ECDH 鍗忓晢涓庨噸浼犲崗鍟嗭拷?
        Bytes ack_payload;
        if (m_config.encryption_enabled) {
            ack_payload.insert(ack_payload.end(),
                               m_local_keypair.public_key.begin(),
                               m_local_keypair.public_key.end());
        }
        ack_payload.push_back(m_config.retransmit_enabled ? 0x01 : 0x00);
        send_control(peer, PacketType::HandshakeAck, ack_payload, true);
    }

    void handle_handshake_ack(Peer* peer, const PacketHeader& header, const Bytes& payload) {
        // Clock sync (瀵硅〃) at handshake time; the command channel also
        // restarts on a fresh session.
        sync_clock(peer, header.tick, unix_millis());
        CommandChannel::reset(*peer);
        // 新会话：重置报告接收时间戳（旧会话的停滞不应在重连握手期
        // 误触发 check_report_timeouts 的 btl 降速）
        peer->m_last_report_at = {};
        // Ack 璐熻浇锛堣嫢鎼哄甫锛夛細[pubkey 32B?][flags 1B]锛屼笌 Handshake 灏鹃儴涓€鑷达拷?
        if (!payload.empty()) {
            std::size_t tail = payload.size();
            peer->m_peer_retransmit = (payload[tail - 1] & 0x01U) != 0;
            tail -= 1;
            if (m_config.encryption_enabled && tail >= 32) {
                std::array<std::uint8_t, 32> peer_pub{};
                std::memcpy(peer_pub.data(), payload.data() + (tail - 32), 32);
                derive_session_key(peer, peer_pub, header.client_id);
            }
        }
        if (peer->m_state == LinkState::Online) return;
        if (peer->m_state == LinkState::Handshake || peer->m_state == LinkState::Established) {
            send_control(peer, PacketType::Online, Bytes{}, true);
            peer->m_last_online_sent = std::chrono::steady_clock::now();
            peer->m_state = LinkState::Online;
            peer->m_handshake_backoff = std::chrono::milliseconds(500);
            fire_peer_event(peer, LinkState::Online);
            send_speed_test(peer);
        }
    }

    void handle_online(Peer* peer, const PacketHeader& header, const Bytes& payload) {
        if (peer->m_state != LinkState::Online) {
            peer->m_state = LinkState::Online;
            fire_peer_event(peer, LinkState::Online);
            send_speed_test(peer);
        }
    }

    void handle_heartbeat(Peer* peer, const PacketHeader& header, const Bytes& payload) {
        if (payload.size() >= 4) {
            std::uint32_t rtt_be = 0;
            std::memcpy(&rtt_be, payload.data(), 4);
            peer->m_sender_rtt_us = to_be32(rtt_be);
        }
        // Every heartbeat doubles as a clock-sync beacon (drift tracking) and
        // as an RTT probe (one-way transit via the per-peer clock offset).
        sync_clock(peer, header.tick, unix_millis());
        feed_rtt_from_tick(peer, header.tick);
    }

    void handle_probe(Peer* peer, const PacketHeader& header) {
        // Speed-test train packet: accumulate wire bytes and arrival span.
        // The train is finalized on a gap (here and in Report::build_payload).
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(peer->m_mu);
        finalize_probe_train(*peer, now);
        if (peer->m_probe_count == 0) peer->m_probe_first = now;
        peer->m_probe_last = now;
        ++peer->m_probe_count;
        peer->m_probe_bytes += kHeaderSize + header.payload_size;
    }

    void send_speed_test(Peer* peer) {
        // Blast a train of blank Probe datagrams to measure the link speed.
        // Bypasses the outbound queue and the token bucket on purpose:
        // pacing the train would measure our own pacing rate, not the link.
        if (!m_config.speed_test_enabled) return;
        if (!peer->m_addr_set || !socket_ready()) return;
        std::size_t frag = m_config.mtu > kHeaderSize ? m_config.mtu - kHeaderSize : 1152;
        std::size_t total = m_config.speed_test_bytes;
        Bytes blank(frag, 0);
        std::size_t sent = 0;
        while (sent < total) {
            std::size_t n = std::min(frag, total - sent);
            PacketHeader header{};
            header.magic = kMagic;
            header.version = kVersion;
            header.type = PacketType::Probe;
            header.client_id = m_local_client_id;
            header.session_id = m_local_session_id;
            header.payload_size = static_cast<std::uint16_t>(n);
            header.tick = static_cast<std::uint32_t>(unix_millis() & 0xFFFFFFFFULL);
            Bytes datagram = PacketCodec::encode(header, Bytes(blank.begin(), blank.begin() + n));
            socket_send(peer->m_addr, datagram.data(), datagram.size());
            sent += n;
        }
    }

    // Clock sync (瀵硅〃): records the peer's clock offset
    // (remote_clock - local_clock, 碌s). The raw sample (remote_tick -
    // local_arrival) contains one-way transit delay, so rtt/2 is subtracted.
    // Both ends keep the offset per peer; local time is never modified.
    // Heartbeats re-sync continuously to track clock drift.
    void sync_clock(Peer* peer, std::uint32_t tick, std::uint64_t arrival_ms) {
        if (tick == 0) return;
        auto rtt_us = m_bandwidth.rtt().count();
        if (rtt_us <= 0) {
            // 灏氭棤 RTT 鏍锋湰锛氭寜鍗曞悜鏃跺欢鐩存帴瀵硅〃锛坮tt/2 淇鏆傝锟?0锛夛拷?
            // 涓嶈兘鎺ㄨ繜鈥斺€攈eartbeat/report 鐨勫線杩旈噰鏍凤紙feed_rtt_from_tick锟?
            // 渚濊禆鏃堕挓鍋忕Щ锛岃€屽亸绉诲張锟?RTT 鏍锋湰锛屾帹杩熶細褰㈡垚寰幆渚濊禆锟?
            // RTT 姘歌繙锟?0锛屽鐩婅皟鑺傚け鏁堬紝甯﹀浼拌鏃犳硶鏀舵暃锟?
            std::uint32_t arrival_low = static_cast<std::uint32_t>(arrival_ms & 0xFFFFFFFFULL);
            std::int64_t sample_us = static_cast<std::int64_t>(
                static_cast<std::int32_t>(tick - arrival_low)) * 1000;
            if (!peer->m_clock_synced) {
                peer->m_clock_offset_us = sample_us;
                peer->m_clock_synced = true;
            } else if (sample_us > peer->m_clock_offset_us) {
                // 只向大收敛：sample = tick-arrival（负值）——链路排队/握手
                // 重传使 sample 更负（伪"时钟差"），向大收敛即冻结在最短
                // 路径校准，不吸收排队与重传污染（-250ms 假偏移会自愈）
                peer->m_clock_offset_us = sample_us;
            }
            peer->m_clock_pending = false;
            return;
        }
        std::uint32_t arrival_low = static_cast<std::uint32_t>(arrival_ms & 0xFFFFFFFFULL);
        std::int64_t sample_us =
            static_cast<std::int64_t>(static_cast<std::int32_t>(tick - arrival_low)) * 1000
            - (rtt_us > 0 && rtt_us < 100000 ? rtt_us / 2 : 0);
        if (!peer->m_clock_synced) {
            peer->m_clock_offset_us = sample_us;
            peer->m_clock_synced = true;
        } else {
            // 只向大收敛：sample = tick - arrival - rtt/2（正常为负值，链路
            // 持续排队/握手重传使 sample 更负——伪"时钟差"）。EMA 跟踪会把
            // 这些伪差吸收（video 数据 transit/P50 恒小 → 迟到线 q=P50-RTprop
            // 归零 → delay-based 拥塞判定失明，无 L4S 时 btl 对带宽下限盲发）。
            // 只向大收敛即冻结在最短路径时钟差，排队与重传污染完整反映到
            // transit（可自愈 -250ms 重传假偏移）。时钟漂移（ppm 级）在会话
            // 时长内误差可忽略；长会话漂移留待慢速校准补充。
            if (sample_us > peer->m_clock_offset_us)
                peer->m_clock_offset_us = sample_us;
        }
        peer->m_clock_pending = false;
    }

    void try_sync_clock(Peer* peer) {
        if (peer->m_clock_pending) {
            sync_clock(peer, peer->m_hs_tick, peer->m_hs_arrival_ms);
        }
    }

    // RTT sample from a control packet's one-way transit: heartbeat/report
    // carry the peer's send tick, and with the per-peer clock offset the
    // one-way transit is measurable; RTT 锟?2 * transit (symmetric path).
    void feed_rtt_from_tick(Peer* peer, std::uint32_t tick) {
        std::int64_t transit_us = transit_time_us(*peer, tick, unix_millis());
        if (transit_us < 0 || transit_us > 10000000) return;  // unsynced / garbage
        m_bandwidth.on_ack(0, std::chrono::microseconds(transit_us * 2));
    }

    void handle_bye(Peer* peer, const PacketHeader& header, const Bytes& payload) {
        if (peer->m_state != LinkState::Closed) {
            peer->m_state = LinkState::Closed;
            fire_peer_event(peer, LinkState::Closed);
        }
    }

    void handle_data(Peer* peer, const PacketHeader& header, const Bytes& payload) {
        std::uint32_t rtt_us = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(m_bandwidth.rtt()).count());
        Reassembler::handle_data(*peer, header, payload, rtt_us,
                                 m_config.late_rtt_multiplier, m_config.late_buffer_ms,
                                 max_message_bytes(), m_config.report_interval,
                                 [this](Peer* p, Bytes message) {
                                     deliver_message(p, std::move(message));
                                 },
                                 [this](Peer* p, std::uint8_t ch) {
                                     TightTransport::MessageLossCallback cb;
                                     {
                                         std::lock_guard<std::mutex> lock(m_callback_mutex);
                                         cb = m_message_loss_cb;
                                     }
                                     if (cb) cb(p->m_id, ch);
                                 });
    }

    void handle_command(Peer* peer, const PacketHeader& header, const Bytes& payload) {
        std::uint32_t rtt_us = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(m_bandwidth.rtt()).count());
        auto ready = CommandChannel::handle(*peer, header, payload, rtt_us);
        for (auto& pl : ready) fire_command(peer, std::move(pl));
    }

    // Periodic command reorder-wait expiry: held commands whose gap has
    // exceeded 3 RTT are delivered (skipping the missing sequence) even when
    // no new commands arrive.
    void flush_commands() {
        std::uint32_t rtt_us = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(m_bandwidth.rtt()).count());
        std::vector<std::pair<Peer*, std::vector<Bytes>>> pending;
        {
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            for (auto& kv : m_peers) {
                auto ready = CommandChannel::flush_expired(kv.second, rtt_us);
                if (!ready.empty()) pending.emplace_back(&kv.second, std::move(ready));
            }
        }
        for (auto& entry : pending) {
            for (auto& pl : entry.second) fire_command(entry.first, std::move(pl));
        }
    }

    void handle_ack(Peer* peer, const PacketHeader& header) {
        std::uint32_t ack = header.acknowledgment;
        if (ack == 0) return;
        std::size_t erased_bytes = 0;
        std::chrono::steady_clock::time_point last_send;
        bool found = false;
        {
            // Ack 报文只携带控制序号（Handshake/HandshakeAck/Online），
            // 只消费控制挂账；数据 pending 由 Report 的 ack 游标剪枝。
            std::lock_guard<std::mutex> lock(peer->m_mu);
            auto it = peer->m_control_pending.find(ack);
            if (it == peer->m_control_pending.end()) return;
            last_send = it->second.m_last_send;
            erased_bytes = it->second.m_bytes;
            peer->m_control_pending.erase(it);
            found = true;
        }
        if (!found) return;
        auto rtt = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - last_send);
        if (rtt.count() < 0) rtt = std::chrono::microseconds(0);
        m_bandwidth.on_ack(erased_bytes, rtt);
        // The ACK RTT sample may be the first one: finish a deferred
        // handshake clock sync.
        try_sync_clock(peer);
    }

    void handle_report(Peer* peer, const PacketHeader& header, const Bytes& payload) {
        TIGHT_DBG_PRINTF("PROBE report-recv peer=%s size=%zu\n", peer->m_id.c_str(), payload.size());
        // RTT sample from the report's one-way transit (clock-offset based).
        feed_rtt_from_tick(peer, header.tick);
        ReportResult r = Report::handle(*peer, payload,
                       [this](Peer* p, const PacketHeader& h, const Bytes& pl) {
                           // 閲嶄紶鐩村彂锛堢粫杩囧嚭绔欓槦鍒椾笌浠ょ墝妗讹級锛氬凡涓㈡姤鏂囧啀
                           // 琚檺閫熸帓闃熷彧浼氭嫋鎱㈠洖澶嶃€佹姮锟?RTT銆傞噸浼犻噺锟?
                           // 锛堢ǔ锟?~4%锛夛紝鐩村彂涓嶇牬鍧忛摼璺檺閫熺殑璇箟锟?
                           if (!p->m_addr_set || !socket_ready()) return;
                           PooledBytes datagram = build_wire_packet(p, h, pl);
                           TIGHT_DBG_PRINTF("PROBE resend seq=%u type=%d\n", (unsigned)h.sequence, (int)h.type);
                           socket_send(p->m_addr, datagram.data(), datagram.size());
                       });
        {
            // 记录报告到达时刻（报告超时检测基准；peer.m_mu 保护，
            // sender 线程读取）
            std::lock_guard<std::mutex> lock(peer->m_mu);
            peer->m_last_report_at = std::chrono::steady_clock::now();
        }
        // 瀵圭涓婃姤鐨勫疄闄呮帴鏀堕€熺巼 锟?鎶曢€掔巼鏍锋湰锛氳 BtlBw 鑳戒粠琚薄鏌撶殑
        // 娴嬮€熸挱绉嶅€间腑鎭㈠锛堟帴鏀剁鐩存帴娴嬮噺锛屼笉锟?ACK 娓告爣璺崇己褰卞搷锟?
        // 绛変簬閾捐矾鐪熷疄鐡堕閫熺巼锛夈€備涪鍖呮牎姝ｏ細鎶曢€掔巼浼氳閾捐矾涓㈠寘鎷変綆锟?
        // 鐩存帴閲囩撼浼氭妸涓㈠寘璇綋鎴愰摼璺閲忥紙btl_bw 锟?(1-L)脳鍙戦€侀€熺巼锟?
        // 1.25脳btl_bw 锟?闇€锟?锟?杈圭晫姝婚攣锛氶檺閫熺粦瀹氣啋鏍锋湰琚嫆鈫掓棤娉曟仮澶嶏級锟?
        // 鏍℃锟?recv_rate/(1-loss)銆傛牎姝ｅ彧閫傜敤浜庡彲鎭㈠鐨勯殢鏈轰涪锟?
        // 锛堚墹25%锛夛細鏇撮珮姣斾緥鐨勪涪鍖呯敱闃熷垪婧㈠嚭涓诲锛堥摼璺湡鐨勫彧鏈夎繖涔堝
        // 瀹归噺锛夛紝鏍℃浼氭妸甯﹀涓嬮檷鍚庣殑鐪熷疄瀹归噺鏀惧ぇ锛屾棤娉曡窡杩涳拷?
        // 三信号 AIMD (GCC style): delay-based + late-based (incl. CE) + ECN.
        // pacer_limited 每报告取一次并复位（本地令牌限速中不判拥塞，防崩底死锁）
        bool pacer_capped = m_pacer_limited.load();
        // 持续超发判定（突刺门控）：报告期平均发送速率 > 对端接收速率 =
        // 持续超发。**send/recv 各自 EWMA 平滑（α=0.5）**：IDR 关键帧突刺
        // （60KB 在 18-24M 链路传输 27-33ms）的 send 计入报告窗口 A、recv
        // 计入窗口 B（到达延迟）——窗口错位让瞬时 send>recv 误判持续超发
        // → CE 突刺照降 → fast 跳帧 → 又 IDR → 循环（实测 47s/52s 反复
        // evac）。EWMA 吸收单窗口尖峰；真正持续超发（多窗口 send>recv）
        // 仍判。recv_rate=0（无流量上报）时保守视为超发。
        bool sustained_overload = true;
        double sent_bytes_in_window = -1.0;
        {
            auto now = std::chrono::steady_clock::now();
            double dt = 0.0;
            if (m_tx_rate_last_ts.time_since_epoch().count() != 0) {
                dt = std::chrono::duration<double>(now - m_tx_rate_last_ts).count();
            }
            std::uint64_t cur = m_tx_bytes.load();
            if (dt > 0.0 && cur >= m_tx_rate_last_bytes) {
                double send_rate = static_cast<double>(cur - m_tx_rate_last_bytes) / dt;
                sent_bytes_in_window =
                    static_cast<double>(cur - m_tx_rate_last_bytes);
                double recv_rate = static_cast<double>(r.recv_rate);
                if (recv_rate > 0) {
                    if (m_overload_send_ewma == 0.0 && m_overload_recv_ewma == 0.0) {
                        m_overload_send_ewma = send_rate;
                        m_overload_recv_ewma = recv_rate;
                    } else {
                        m_overload_send_ewma = 0.5 * m_overload_send_ewma + 0.5 * send_rate;
                        m_overload_recv_ewma = 0.5 * m_overload_recv_ewma + 0.5 * recv_rate;
                    }
                    sustained_overload = m_overload_send_ewma > m_overload_recv_ewma;
                    // 超发积压累计（slow 排空 Q 的数据源）：send−recv 正差值积分
                    if (m_overload_send_ewma > m_overload_recv_ewma) {
                        std::lock_guard<std::mutex> lk(m_cap_mu);
                        m_evac_pending += static_cast<std::uint64_t>(
                            (m_overload_send_ewma - m_overload_recv_ewma) * dt);
                    }
                } else {
                    // recv_rate=0：EWMA 复位（无流量上报期后重新初始化）
                    m_overload_send_ewma = 0.0;
                    m_overload_recv_ewma = 0.0;
                }
            }
            m_tx_rate_last_bytes = cur;
            m_tx_rate_last_ts = now;
        }
        // 帧级迟到率（发送端用 btl 折算合理到达时间 = F/btl + late_buffer）：
        //   关键帧突刺（I 帧 40-60KB 在低带宽链路传输数十 ms）是帧自身传输，
        //   不算迟到；持续超发排队超过最大帧合理时间才记迟到。
        //   lite 模式：线 = F_max/btl + buffer，直方图超线帧占比
        //   正常模式：逐帧线 = F_i/btl + buffer，逐帧判定
        double frame_late = -1.0;   // -1 = 本报告无帧统计（回退报文级 late）
        {
            // 线基准 = 对端 P50（固定链路/处理延迟）+ 帧传输时间（F/btl）+
            // late_buffer：关键帧越大线越宽（帧自身传输不算迟到）；固定
            // 延迟（10ms 代理等）由 P50 承载，不误判正常帧。
            double p50_us = static_cast<double>(peer->m_peer_p50_ms) * 1000.0;
            double btl_bps = static_cast<double>(m_bandwidth.btl_bw_bps()) * 8.0;
            double buffer_us = static_cast<double>(m_config.late_buffer_ms) * 1000.0;
            if (r.m_frame_lite) {
                if (r.m_frame_hist_samples > 0 && r.m_frame_max_bytes > 0 && btl_bps > 0.0) {
                    double line_us = p50_us +
                        static_cast<double>(r.m_frame_max_bytes) * 8.0 / btl_bps * 1e6 + buffer_us;
                    std::uint64_t over = 0;
                    for (std::size_t b = 0; b < r.m_frame_hist.size(); ++b) {
                        double bin_us = static_cast<double>(b) * 8000.0 + 4000.0;
                        if (bin_us > line_us) over += r.m_frame_hist[b];
                    }
                    frame_late = static_cast<double>(over) /
                                 static_cast<double>(r.m_frame_hist_samples);
                    if (frame_late > 1.0) frame_late = 1.0;
                }
            } else {
                if (!r.m_frame_pairs.empty() && btl_bps > 0.0) {
                    std::uint64_t late_cnt = 0;
                    for (auto& fp : r.m_frame_pairs) {
                        double line_us = p50_us +
                            static_cast<double>(fp.first) * 8.0 / btl_bps * 1e6 + buffer_us;
                        if (static_cast<double>(fp.second) * 1000.0 > line_us) ++late_cnt;
                    }
                    frame_late = static_cast<double>(late_cnt) /
                                 static_cast<double>(r.m_frame_pairs.size());
                }
            }
        }
        if (frame_late >= 0.0) {
            std::lock_guard<std::mutex> lock(peer->m_mu);
            peer->m_peer_late_ratio = frame_late;   // FEC 驱动与拥塞判定用帧级值
        }
        // fast 排空窗口完成确认（保留，供扩展）：新 IDR 已提交发送 && 本
        // 报告到达 = 排空完成、跳帧落地。btl 冻结期独立于窗口状态（触发+3s）
        // ——即使排空窗口已确认完成，锁定期未到仍冻结（刚跳帧迟到信号失真，
        // 不能立即爬升）。
        if (m_evac_mode.load() == 1 && m_evac_keyframe_sent.load()) {
            m_evac_mode.store(0);
            m_evac_keyframe_sent.store(false);
        }
        // btl 冻结 = slow 排空窗口（3s Q 面积法）‖ fast 冻结期（触发+3s，
        // IDR 提交时延长 kKeyframeHoldTotal——关键帧超发 CE 豁免并入）
        auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        auto freeze_ns = m_evac_freeze_until_ns.load();
        bool in_evac = (m_evac_mode.load() == 2 &&
                        std::chrono::steady_clock::now() < m_evac_slow_deadline) ||
                       (freeze_ns != 0 && now_ns < static_cast<std::int64_t>(freeze_ns));
        m_bandwidth.on_report(peer->m_peer_p50_ms,
                              frame_late >= 0.0 ? frame_late : peer->m_peer_late_ratio,
                              static_cast<double>(r.loss_ratio) / 10000.0,
                              static_cast<double>(r.ce_ratio) / 10000.0,
                              static_cast<std::uint32_t>(m_bandwidth.rtt().count()),
                              pacer_capped, sustained_overload, in_evac,
                              static_cast<double>(r.recv_rate),
                              sent_bytes_in_window);
        // probe 起步校准：握手后首个测速报告到达 → 种子设为实测链路容量、
        // btl 不超链路（bandwidth 内部只校准一次）。probe_bw 单位 bps。
        if (r.probe_bw > 0) {
            m_bandwidth.set_seed_and_clamp(r.probe_bw);
        }
        // FEC 关闭条件（让出只限冗余"无用/有害"的场景）：
        //  RTT>200ms：长距离/重拥塞
        //  CE 活跃（>1%）：L4S——CE 即 loss（CE=loss，网络不丢包只标记），
        //    冗余无用且加剧排队；CE 场景的本地排队/排队型拥塞也由 CE 表达
        // 丢包型场景（随机丢包）**不关**：丢包正是 FEC 的工作对象，且
        // video_capacity 预算已按实际冗余率折算（冗余不额外超发）。曾按
        // loss/delay/pacer 组合关闭，实测误伤：丢包稀疏（333ms 报告内
        // 常为 0）→ FEC 在丢包间隔关闭 → 无法恢复 → 64 帧 nokey（vs
        // 全开 1 帧）。
        peer->m_fec_disable.store(
            m_bandwidth.rtt() > std::chrono::milliseconds(kFecDisableRttMs) ||
            r.ce_ratio > 100);   // CE > 1%（kCeThreshold）
        // 视频容量迟滞通知（btl/冗余率更新后，变化 >10% 且 >100k 才推）
        notify_video_capacity();
    }

    void send_control(Peer* peer, PacketType type, const Bytes& payload, bool ackable) {
        PacketHeader header{};
        header.magic = kMagic;
        header.version = kVersion;
        header.type = type;
        header.client_id = m_local_client_id;
        header.session_id = m_local_session_id;
        if (ackable) {
            header.sequence = peer->m_control_seq_out++;
        }
        header.payload_size = static_cast<std::uint16_t>(payload.size());
        header.tick = static_cast<std::uint32_t>(unix_millis() & 0xFFFFFFFFULL);
        Bytes datagram = PacketCodec::encode(header, payload);
        send_raw(peer, datagram);
        if (ackable) {
            auto& ps = peer->m_control_pending[header.sequence];
            ps.m_header = header;
            ps.m_payload = payload;
            ps.m_last_send = std::chrono::steady_clock::now();
            ps.m_bytes = datagram.size();
        }
    }

    void send_ack_packet(Peer* peer, std::uint32_t ack) {
        PacketHeader header{};
        header.magic = kMagic;
        header.version = kVersion;
        header.type = PacketType::Ack;
        header.client_id = m_local_client_id;
        header.session_id = m_local_session_id;
        header.acknowledgment = ack;
        header.tick = static_cast<std::uint32_t>(unix_millis() & 0xFFFFFFFFULL);
        Bytes datagram = PacketCodec::encode(header, Bytes{});
        send_raw(peer, datagram);
    }

    void send_handshake(Peer* peer) {
        Bytes payload;
        payload.push_back(static_cast<std::uint8_t>(m_config.role));
        auto id_size = static_cast<std::uint16_t>(
            std::min<std::size_t>(m_config.id.size(), 65535U));
        payload.push_back(static_cast<std::uint8_t>((id_size >> 8U) & 0xFFU));
        payload.push_back(static_cast<std::uint8_t>(id_size & 0xFFU));
        payload.insert(payload.end(), m_config.id.begin(), m_config.id.begin() + id_size);
        payload.insert(payload.end(), m_config.token.begin(), m_config.token.end());
        // 杩藉姞鏈湴 X25519 鍏挜锛屼緵瀵圭 ECDH 鍗忓晢浼氳瘽瀵嗛挜
        if (m_config.encryption_enabled) {
            payload.insert(payload.end(), m_local_keypair.public_key.begin(),
                           m_local_keypair.public_key.end());
        }
        // 灏鹃儴 1 瀛楄妭鑳藉姏鏍囧織锛歜it0 = retransmit_enabled 閫氬憡锟?
        // 瀵圭鎹鍐冲畾鏄惁涓烘湰閾捐矾淇濈暀閲嶄紶缂撳啿锛堜换涓€绔彲鍗曟柟闈㈠叧闂級锟?
        payload.push_back(m_config.retransmit_enabled ? 0x01 : 0x00);
        peer->m_last_handshake_sent = std::chrono::steady_clock::now();
        send_control(peer, PacketType::Handshake, payload, true);
    }

    // ECDH 鍗忓晢锟?HKDF 娲剧敓浼氳瘽瀵嗛挜锛氬弻锟?client_id 鎺掑簭鎷兼帴浣滀负 salt锟?
    // 涓ょ鍥犳瀵煎嚭鐩稿悓锟?AES-256-GCM 瀵嗛挜锟?
    void derive_session_key(Peer* peer, const std::array<std::uint8_t, 32>& peer_pub,
                            std::uint32_t peer_client_id) {
        std::array<std::uint8_t, 32> shared{};
        if (!x25519(shared, m_local_keypair.private_key, peer_pub)) {
            TIGHT_LOG_WARN("[tight] ECDH 浣庨樁鐐癸紝璇ュ绔笉鍚敤鍔犲瘑");
            return;
        }
        std::uint32_t lo = std::min(m_local_client_id, peer_client_id);
        std::uint32_t hi = std::max(m_local_client_id, peer_client_id);
        std::array<std::uint8_t, 8> salt{};
        for (int i = 0; i < 4; ++i) {
            salt[i] = static_cast<std::uint8_t>((lo >> (i * 8)) & 0xFF);
            salt[4 + i] = static_cast<std::uint8_t>((hi >> (i * 8)) & 0xFF);
        }
        peer->m_crypto_key = hkdf_sha256(shared.data(), shared.size(),
                                         salt.data(), salt.size(), "tight-data-key-v1");
        peer->m_crypto_ready = true;
    }

    // GCM 96 锟?nonce锛堟瘡鍙戦€佹柟鍚戝敮涓€锛夛細
    //   Data/Parity: client_id(4) | message_id(4) | fragment_index(2) | type(1) | 0(1)
    //   Command:     client_id(4) | sequence(4)   | type(1) | 0(3)
    // 瀛楁鎸夊ぇ绔啓鍏ワ紝淇濊瘉涓嶅悓瀛楄妭搴忕殑涓绘満闂翠竴鑷达拷?
    std::array<std::uint8_t, kGcmNonceSize> build_nonce(const PacketHeader& header) {
        std::array<std::uint8_t, kGcmNonceSize> n{};
        auto put32 = [&](std::size_t off, std::uint32_t v) {
            std::uint32_t be = to_be32(v);
            std::memcpy(n.data() + off, &be, 4);
        };
        put32(0, header.client_id);
        if (header.type == PacketType::Command) {
            put32(4, header.sequence);
            n[8] = static_cast<std::uint8_t>(header.type);
        } else {
            put32(4, header.message_id);
            std::uint16_t be_idx = to_be16(header.fragment_index);
            std::memcpy(n.data() + 8, &be_idx, 2);
            n[10] = static_cast<std::uint8_t>(header.type);
        }
        return n;
    }

    // 鐢辨槑鏂囪礋杞藉崟缂撳啿鏋勫缓绾夸笂鎶ユ枃锛堟睜鍖栵級锟?
    // 澶撮儴 锟?鏄庢枃/瀵嗘枃璐熻浇 锟?CRC 瀹氱锛屼竴娆″垎閰嶅畬鎴愶紱
    // 鍔犲瘑锟?AES-256-GCM 鐩存帴瀵嗗啓杩涙姤鏂囪礋杞藉尯锛圓AD = 澶撮儴锟?44 瀛楄妭锛夛拷?
    PooledBytes build_wire_packet(Peer* peer, const PacketHeader& header, const Bytes& payload) {
        PacketHeader wire_header = header;
        bool encrypted = m_config.encryption_enabled && peer->m_crypto_ready;
        if (encrypted) {
            wire_header.flags |= kFlagEncrypted;
            wire_header.payload_size = static_cast<std::uint16_t>(payload.size() + kGcmTagSize);
        }
        PooledBytes datagram(kHeaderSize + wire_header.payload_size);
        PacketCodec::encode_header_to(wire_header, datagram.data());
        if (!encrypted) {
            if (!payload.empty()) {
                std::memcpy(datagram.data() + kHeaderSize, payload.data(), payload.size());
            }
        } else {
            auto nonce = build_nonce(wire_header);
            aes256_gcm_encrypt(peer->m_crypto_key, nonce,
                               datagram.data(), kHeaderSize - 4,   // AAD锛氬ご閮ㄥ墠 44 瀛楄妭
                               payload.data(), payload.size(),
                               datagram.data() + kHeaderSize,      // 瀵嗘枃
                               datagram.data() + kHeaderSize + payload.size());  // 16B 鏍囩
        }
        PacketCodec::finalize_crc(datagram.data(), datagram.size());
        return datagram;
    }

    // 瑙ｅ瘑璐熻浇骞舵牎楠岃璇佹爣绛撅紱澶辫触杩斿洖 false锛堣皟鐢ㄦ柟涓㈠純鎶ユ枃锟?
    bool decrypt_payload(Peer* peer, PacketHeader& header, Bytes& payload) {
        if (!peer->m_crypto_ready) return false;
        if (payload.size() < kGcmTagSize) return false;
        std::size_t ct_len = payload.size() - kGcmTagSize;
        auto nonce = build_nonce(header);
        // AAD 鏍堢紦鍐茬紪鐮侊紙闆跺爢鍒嗛厤锟?
        std::uint8_t aad[kHeaderSize];
        PacketCodec::encode_to(header, Bytes{}, aad);
        Bytes pt(ct_len);
        if (!aes256_gcm_decrypt(peer->m_crypto_key, nonce, aad, kHeaderSize - 4,
                                payload.data(), ct_len, payload.data() + ct_len,
                                pt.data())) {
            return false;
        }
        payload = std::move(pt);
        header.payload_size = static_cast<std::uint16_t>(ct_len);
        header.flags &= ~kFlagEncrypted;   // 鎭㈠ flags 浣庝綅璇箟锛堟暟鎹垎鐗囨暟锟?
        return true;
    }

    void send_data_packet(Peer* peer, std::uint32_t msg_id, std::uint16_t idx,
                          std::uint16_t cnt, std::uint16_t data_cnt,
                          std::uint16_t real_size,
                          const std::uint8_t* frag_data, std::size_t frag_len,
                          std::size_t width, bool ackable,
                          std::uint8_t channel = 0, bool keyframe = false) {
        // 绾夸笂鍒嗙墖璐熻浇缁熶竴锟?width锛堜笉瓒宠ˉ闆讹級锛屼笌鎺ユ敹锟?RS 鎭㈠瀵归綈
        Bytes payload(width, 0);
        if (frag_data && frag_len > 0) {
            std::memcpy(payload.data(), frag_data, std::min(frag_len, width));
        }
        PacketHeader header{};
        header.magic = kMagic;
        header.version = kVersion;
        // 类型判定以"是否校验片"为准：所有数据分片（含单分片消息）都
        // 是 Data（分配 seq、参与缺口跟踪与 ARQ 重传）；只有真正的校验
        // 片（idx >= data_cnt）才是 Parity（seq=0，不参与跟踪——校验片
        // 丢失时数据片齐即可组装，无需重传）。
        header.type = (idx < data_cnt) ? PacketType::Data : PacketType::Parity;
        header.flags = data_cnt;
        bool is_data = (header.type == PacketType::Data);
        header.client_id = m_local_client_id;
        header.session_id = m_local_session_id;
        bool is_acked_data = ackable && is_data;
        // 閲嶄紶缂撳啿锛氫粎鍙潬閫氶亾锛坧er-channel 寮€鍏筹級淇濈暀锛涗粛闇€瀵圭鎻℃墜閫氬憡
        // 鏀寔閲嶄紶锛堜换涓€绔崟鏂归潰鍏抽棴鍗充笉鍐嶄负鏈摼璺紦鍐诧紝锟?FEC 鍏滃簳锛夛拷?
        bool reliable_ch = (channel < 8) && m_config.channel_reliable[channel];
        bool keep_pending = is_acked_data && reliable_ch && peer->m_peer_retransmit;
        // Brief lock: only for sequence assignment and pending insertion.
        // send_raw is called WITHOUT any lock, so the reactor thread can run
        // even when this thread is blocked on token-bucket back-pressure.
        {
            std::lock_guard<std::mutex> lock(peer->m_mu);
            if (is_acked_data) {
                header.sequence = peer->m_sequence_out++;
            }
            header.message_id = msg_id;
            header.fragment_index = idx;
            header.fragment_count = cnt;
            // reserved 锟?4 浣嶅瓨閫氶亾鍙凤紝锟?12 浣嶅瓨 real_size
            header.reserved = static_cast<std::uint16_t>(
                (real_size & kRealSizeMask) | ((channel & 0x0F) << kChannelShift) |
                (keyframe ? kKeyframeMask : 0));
            header.payload_size = static_cast<std::uint16_t>(payload.size());
            header.tick = static_cast<std::uint32_t>(unix_millis() & 0xFFFFFFFFULL);
            if (keep_pending) {
                auto& ps = peer->m_pending[header.sequence];
                ps.m_header = header;
                ps.m_payload = payload;
                ps.m_last_send = std::chrono::steady_clock::now();
                ps.m_bytes = 0;  // filled in after encode to avoid re-encoding
            }
        }
        auto datagram = build_wire_packet(peer, header, payload);
        std::size_t wire_size = datagram.size();
        send_raw(peer, std::move(datagram), channel);
        if (keep_pending) {
            std::lock_guard<std::mutex> lock(peer->m_mu);
            peer->m_pending[header.sequence].m_bytes = wire_size;
        }
    }

    // 发送通道统一出口：外部 socket 模式经 ISocket::send_to() 输出，
    // 默认模式走内部 UDP socket。所有数据报发送点都经此函数。
    bool socket_ready() const {
        return m_ext_socket != nullptr || m_sock != kInvalidSocket;
    }
    void socket_send(const sockaddr_in& to, const void* data, std::size_t len) {
        if (m_ext_socket) {
            NetAddress addr{address_host(to), ntohs(to.sin_port)};
            m_ext_socket->send_to(addr, static_cast<const std::uint8_t*>(data), len);
            return;
        }
        if (m_sock == kInvalidSocket) return;
        tight_sendto(m_sock, static_cast<const char*>(data), static_cast<int>(len), 0,
                     reinterpret_cast<const sockaddr*>(&to),
                     static_cast<int>(sizeof(to)));
    }

    // Enqueue an outbound packet. The sender thread will do the actual sendto
    // (with token-bucket back-pressure). This MUST NOT block the caller.
    void send_raw(Peer* peer, PooledBytes datagram, std::uint8_t channel = 0) {
        if (!peer->m_addr_set || !socket_ready()) return;
        // 总发送字节在 sender_loop/drain_sender 实际出队发送处累计
        // （m_tx_bytes）：超发积压累计须统计真实线上流量，入队侧会混入
        // 背压滞留。
        // file/data 通道（2/3）发送字节累计：video_capacity_bps 扣除用
        if (channel == 2 || channel == 3) {
            m_fd_tx_bytes.fetch_add(datagram.size());
        }
        // 音频通道（1）入独立队列：sender 优先清空、绕过令牌桶
        BlockingQueue<OutboundPacket>& queue =
            (channel == 1) ? m_audio_queue : m_outbound_queue;
        if (!queue.try_push(OutboundPacket{peer, channel, std::move(datagram)})) {
            // 出站队列满被静默丢弃（丢包定位）
        }
    }

    // 鎺у埗鎶ユ枃鐩村彂锛氭姤锟?鍛戒护缁曡繃鍑虹珯闃熷垪涓庝护鐗屾《锛岀珛锟?sendto锟?
    // 鐞嗙敱锛氬弽鍚戦€氶亾琚暟鎹椽娉涙尋鍗犳椂锛屾姤鍛婂湪 echo 娲硾鍚庢帓 ~7s 锟?
    // 鍒拌揪瀵圭锛堝疄娴嬶級锛屾姇閫掔巼鏍锋湰杩囨湡 锟?btl 璺熻穼琚牱鏈欢杩熸嫋鎱拷?
    // 鎶ュ憡 1/s銆佸懡浠や綆棰戜笖閮芥槸鏁扮櫨瀛楄妭锛岀洿鍙戜笉鐮村潖閾捐矾闄愰€熻涔夛拷?
    // 娉細閲嶄紶鐩村彂锛坔andle_report 锟?resend 鍥炶皟锛夋棭宸查噰鐢ㄥ悓涓€妯″紡锟?
    void send_direct(Peer* peer, PooledBytes datagram) {
        if (!peer->m_addr_set || !socket_ready()) return;
        socket_send(peer->m_addr, datagram.data(), datagram.size());
    }
    void send_direct(Peer* peer, const Bytes& datagram) {
        if (!peer->m_addr_set || !socket_ready()) return;
        socket_send(peer->m_addr, datagram.data(), datagram.size());
    }

    // 鎺у埗璺緞锛堜綆棰戯級渚挎嵎閲嶈浇锛氭櫘锟?Bytes 杞睜鍖栫紦鍐插叆锟?
    void send_raw(Peer* peer, const Bytes& datagram, std::uint8_t channel = 7) {
        if (!peer->m_addr_set || !socket_ready()) return;
        m_outbound_queue.try_push(
            OutboundPacket{peer, channel, PooledBytes(datagram.begin(), datagram.end())});
    }

    void receiver_loop() {
        // Continuously recvfrom and dispatch to handle_packet. This
        // guarantees the reactor thread is never blocked on socket I/O.
        std::uint8_t buf[2048];
        while (m_running.load(std::memory_order_acquire) &&
               m_workers_running.load(std::memory_order_acquire)) {
            sockaddr_in from{};
            SockLen flen = sizeof(from);
            int n = tight_recvfrom(m_sock, reinterpret_cast<char*>(buf),
                                   static_cast<int>(sizeof(buf)), 0,
                                   reinterpret_cast<sockaddr*>(&from), &flen);
            if (n < 0) {
                int e = last_socket_error();
                if (would_block(e)) {
                    // Avoid busy-spin: short sleep then retry.
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                    continue;
                }
                // Other error: brief sleep to avoid hot loop, then retry.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            // L4S CE 鏍囪鏁版嵁鎶ワ紙proxy 鐩存帴涓嬪彂锛夛細璁℃暟鍚庤烦杩囪В鐮侊拷?
            if (tight_detail::ecn::is_ce_mark(buf, n)) {
                Peer* peer = find_or_create_peer_by_addr(from);
                std::lock_guard<std::mutex> lk(peer->m_mu);
                peer->m_ce_marks += 1;
                continue;
            }
            if (static_cast<std::size_t>(n) < kHeaderSize) continue;
            PacketHeader header{};
            Bytes payload;
            // 鐩存帴浠庢爤缂撳啿瑙ｇ爜锛堟祦锟?CRC锛夛紝鍏嶅幓 datagram 鎷疯礉
            if (!PacketCodec::decode(buf, static_cast<std::size_t>(n), header, payload)) continue;
            handle_packet(from, header, payload);
        }
    }

    // 外部 socket 模式：应用注入一个完整数据报（在应用收包线程内同步
    // 完成解码与分发，等价于 receiver_loop 的单次迭代）。
    void inject_packet(const NetAddress& from, const std::uint8_t* data, std::size_t size) {
        if (!m_ext_socket || !m_running.load(std::memory_order_acquire)) return;
        if (!data || size == 0) return;
        sockaddr_in addr{};
        if (!resolve_address(from.host, from.port, addr)) return;
        // L4S CE 标记数据报：仅计数（同 receiver_loop）
        if (tight_detail::ecn::is_ce_mark(data, static_cast<int>(size))) {
            Peer* peer = find_or_create_peer_by_addr(addr);
            std::lock_guard<std::mutex> lk(peer->m_mu);
            peer->m_ce_marks += 1;
            return;
        }
        if (size < kHeaderSize) return;
        PacketHeader header{};
        Bytes payload;
        if (!PacketCodec::decode(data, size, header, payload)) return;
        handle_packet(addr, header, std::move(payload));
    }

    // 报告超时检测（reactor 节拍调用，函数内 500ms 时间门控防高频空转）：
    // 对端持续收不到报告（3×report_interval = 链路严重卡顿/断流）→ btl ×0.5
    // 单次降（one-shot，报告恢复后恢复台阶自然回升），下限防打穿。
    // 门控条件：
    //  - 仅 Online/Established 链路（与 send_reports 一致；重连握手期
    //    不判——m_last_report_at 也在握手时重置，双保险）；
    //  - 已收到过首个报告（m_have_late_report，防握手期误触发）；
    //  - 全部活跃 peer 均停滞才降（m_bandwidth 是全局估计器——网关多设备
    //    时单设备停摆不应拖累整条链路的 btl；单链路场景行为不变）。
    void check_report_timeouts() {
        static auto s_last_check = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        if (s_last_check.time_since_epoch().count() != 0 &&
            now - s_last_check < std::chrono::milliseconds(500)) {
            return;
        }
        s_last_check = now;
        auto timeout = m_config.report_interval * 3;
        std::size_t active = 0;
        std::size_t stalled = 0;
        {
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            for (auto& kv : m_peers) {
                auto& peer = kv.second;
                if (peer.m_state != LinkState::Established &&
                    peer.m_state != LinkState::Online) {
                    continue;
                }
                ++active;
                bool timed_out = false;
                {
                    std::lock_guard<std::mutex> plk(peer.m_mu);
                    if (peer.m_have_late_report &&
                        now - peer.m_last_report_at >= timeout) {
                        timed_out = true;
                    }
                }
                if (timed_out) ++stalled;
            }
        }
        if (active > 0 && stalled == active) {
            // 冻结期（fast 排空 + 锁定）内豁免：刚快排跳帧，报告缺失/迟到
            // 是正常现象（链路在排空，IDR 跳帧中），超时降速（×0.5）会
            // 破坏冻结语义——冻结期最多 3s，链路真断 3s 后照常检测。
            auto freeze_ns = m_evac_freeze_until_ns.load();
            bool frozen = freeze_ns != 0 &&
                          now.time_since_epoch().count() < static_cast<std::int64_t>(freeze_ns);
            if (!frozen) {
                m_bandwidth.on_report_timeout();
            }
        }
    }

    void sender_loop() {
        // 心跳化发送（10ms）：每心跳结算令牌（空闲心跳也 refill = 自动还贷），        // 然后按优先级发送：① 音频（独立队列，绕过令牌，一次性发完）
        // ② 视频（共享令牌桶：足额消费或贷款透支——帧随心跳连发，不被打平）
        // ③ file/data（严格令牌：带宽分配器，不足则等下一心跳）。
        // 贷款耗尽（token < −btl×loan_seconds）→ 清空积压 + 持续排空视频通道
        // + 回调应用；债务清零（token ≥ 0）→ 恢复发送 + 回调（应用重启编码
        // 器出关键帧）。
        constexpr auto kHeartbeat = std::chrono::milliseconds(10);
        auto hb_start = std::chrono::steady_clock::now();
        while (m_running.load(std::memory_order_acquire) &&
               m_workers_running.load(std::memory_order_acquire)) {
            refill_token_bucket();   // 每心跳结算（token 负值自动补正 = 还贷）
            // fast 排空 = 破产清算：队列已清（clear_outbound），token 债务
            // 一并清零（还债对象消失）、解除贷款排空（否则新 IDR 被
            // loan_drain 丢弃、拖到债务还清才发出）——新 IDR 以全新贷款
            // 额度（btl×5s）连发，播放端快速跳帧、追赶消除。
            if (m_evac_reset_token.exchange(false)) {
                m_token_bucket = 0.0;
                m_loan_drain.store(false);
                m_loan_exhausted_reported = false;
            }
            // 本地限速标记（持续状态，on_report 只读不复位）：token<0 =
            // 视频透支/贷款中/债务未清——本地令牌不足（btl < 视频码率下限
            // 时 1.5M 视频永远超出令牌），播放端帧慢导致的 late/loss 是伪
            // 信号，不能判成链路拥塞（否则"本地慢 → cong → 不恢复爬升 →
            // 贷款不恢复"死锁）。队列深（≥32）也是本地背压。
            m_pacer_limited.store(m_token_bucket < 0 || m_outbound_queue.size() >= 32);
            // ① 音频队列：绕过令牌桶，一次性发完（实时音频无条件优先）
            for (;;) {
                auto opt = m_audio_queue.poll();
                if (!opt) break;
                OutboundPacket pkt = std::move(*opt);
                auto now_ms = unix_millis();
                if (pkt.m_channel < 8 && now_ms < m_drain_until_ms[pkt.m_channel].load()) {
                    continue;
                }
                if (!pkt.m_peer->m_addr_set || !socket_ready()) continue;
                m_tx_bytes.fetch_add(pkt.m_datagram.size());
                socket_send(pkt.m_peer->m_addr, pkt.m_datagram.data(),
                            pkt.m_datagram.size());
            }
            // ②③ 普通队列（视频 ch0 + file/data ch2/3）
            bool loan_exhausted_now = false;
            for (;;) {
                auto opt = m_outbound_queue.poll();
                if (!opt) break;
                OutboundPacket pkt = std::move(*opt);
                auto now_ms = unix_millis();
                if (pkt.m_channel < 8 && now_ms < m_drain_until_ms[pkt.m_channel].load()) {
                    continue;
                }
                if (pkt.m_channel == 0 && m_loan_drain.load()) continue;  // 贷款耗尽期视频排空
                if (!pkt.m_peer->m_addr_set || !socket_ready()) continue;
                double cost = static_cast<double>(pkt.m_datagram.size());
                if (pkt.m_channel == 0) {
                    // 视频：足额消费或贷款透支（不 sleep 打平——帧分片连发）
                    if (m_token_bucket - cost >= -loan_limit()) {
                        m_token_bucket -= cost;
                        m_tx_bytes.fetch_add(pkt.m_datagram.size());
                        socket_send(pkt.m_peer->m_addr, pkt.m_datagram.data(),
                                    pkt.m_datagram.size());
                    } else {
                        loan_exhausted_now = true;   // 贷款耗尽：硬止损
                        break;
                    }
                } else {
                    // file/data：严格令牌（带宽分配），不足等下一心跳
                    if (m_token_bucket >= cost) {
                        m_token_bucket -= cost;
                        m_tx_bytes.fetch_add(pkt.m_datagram.size());
                        socket_send(pkt.m_peer->m_addr, pkt.m_datagram.data(),
                                    pkt.m_datagram.size());
                    } else {
                        // 令牌不足（发送被限速）即标记本地限速中——AIMD 据此
                        // 区分"本地令牌排队"与"链路拥塞"（p50 高时前者走恢复
                        // 台阶）。余包留队，下一心跳再试。
                        m_pacer_limited.store(true);
                        break;
                    }
                }
            }
            if (loan_exhausted_now) {
                // 贷款耗尽：清空剩余积压（file/data 有 ARQ 重传兜底），
                // 持续排空视频通道至债务清零；通知应用（只置标志，应用
                // 在恢复回调时重启编码器——否则新 IDR 也会被排空丢弃）。
                // pacer_limited 已由每心跳统一设置（token<0 = 本地受限）。
                while (auto pkt = m_outbound_queue.poll()) {}
                m_loan_drain.store(true);
                if (!m_loan_exhausted_reported) {
                    m_loan_exhausted_reported = true;
                    notify_loan_exhausted(true);
                }
            }
            if (m_loan_drain.load() && m_token_bucket >= 0.0) {
                // 债务清零：恢复视频发送 + 通知应用（重启编码器出关键帧）
                m_loan_drain.store(false);
                m_loan_exhausted_reported = false;
                notify_loan_exhausted(false);
            }
            // 心跳对齐（10ms）
            hb_start += kHeartbeat;
            auto now = std::chrono::steady_clock::now();
            if (now < hb_start) {
                std::this_thread::sleep_until(hb_start);
            } else {
                hb_start = now;
            }
        }
        // Drain remaining packets on shutdown.
        while (true) {
            auto opt = m_outbound_queue.poll();
            if (!opt) break;
        }
        while (true) {
            auto opt = m_audio_queue.poll();
            if (!opt) break;
        }
    }

    void send_handshakes() {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto now = std::chrono::steady_clock::now();
        for (auto& kv : m_peers) {
            auto& peer = kv.second;
            if (peer.m_state != LinkState::Handshake) continue;
            // 握手报文是 ackable 的：对端不在时挂账永远留在
            // m_control_pending（收不到 ACK），重发前清掉陈旧握手挂账，
            // 避免无限滞留（重传由退避循环驱动，不经挂账）。
            {
                std::lock_guard<std::mutex> plk(peer.m_mu);
                for (auto it = peer.m_control_pending.begin();
                     it != peer.m_control_pending.end();) {
                    if (it->second.m_header.type == PacketType::Handshake) {
                        it = peer.m_control_pending.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (now - peer.m_last_handshake_sent < peer.m_handshake_backoff) continue;
            send_handshake(&peer);
            peer.m_handshake_backoff = std::min(peer.m_handshake_backoff * 2,
                                                std::chrono::milliseconds(5000));
        }
    }

    void send_heartbeats() {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto now = std::chrono::steady_clock::now();
        for (auto& kv : m_peers) {
            auto& peer = kv.second;
            if (peer.m_state != LinkState::Established && peer.m_state != LinkState::Online) continue;
            if (now - peer.m_last_heartbeat_sent < m_config.heartbeat) continue;
            Bytes hb_payload(4);
            std::uint32_t rtt_us = static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(m_bandwidth.rtt()).count());
            std::uint32_t rtt_be = to_be32(rtt_us);
            std::memcpy(hb_payload.data(), &rtt_be, 4);
            send_control(&peer, PacketType::Heartbeat, hb_payload, false);
            peer.m_last_heartbeat_sent = now;
        }
    }

    // Established/Online 鐘舵€佸懆鏈熼噸锟?Online 閫氬憡锛堝箓绛夛細瀵圭 handle_online
    // 浠呭湪鐘舵€佽縼绉绘椂鐢熸晥锛岄噸澶嶅埌杈炬棤瀹筹級銆傛彙鎵嬪悗鑻ュ鏂圭殑 Online 閫氬憡锟?
    // 寮辩綉涓涪澶憋紙瀹炴祴 20% 涓㈠寘锟?node 浼氭案涔呭崱锟?Established锛夛紝鏈€澶氫竴锟?
    // 蹇冭烦鍛ㄦ湡鍚庨噸鍙戝埌杈撅紝鍙屾柟搴旂敤灞傜姸鎬佽嚜鐒舵敹鏁涳拷?
    void send_online_announcements() {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto now = std::chrono::steady_clock::now();
        for (auto& kv : m_peers) {
            auto& peer = kv.second;
            if (peer.m_state != LinkState::Established && peer.m_state != LinkState::Online) continue;
            if (now - peer.m_last_online_sent < m_config.heartbeat) continue;
            send_control(&peer, PacketType::Online, Bytes{}, false);
            peer.m_last_online_sent = now;
        }
    }

    void send_reports() {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto now = std::chrono::steady_clock::now();
        for (auto& kv : m_peers) {
            auto& peer = kv.second;
            if (peer.m_state != LinkState::Established && peer.m_state != LinkState::Online) continue;
            if (now - peer.m_last_report_sent < m_config.report_interval) continue;
            Bytes payload = Report::build_payload(peer, m_config.report_interval,
                                                  m_config.late_buffer_ms,
                                                  m_lite_mode.load());

            PacketHeader rpt{};
            rpt.magic = kMagic;
            rpt.version = kVersion;
            rpt.type = PacketType::Report;
            rpt.client_id = m_local_client_id;
            rpt.session_id = m_local_session_id;
            rpt.payload_size = static_cast<std::uint16_t>(payload.size());
            rpt.tick = static_cast<std::uint32_t>(unix_millis() & 0xFFFFFFFFULL);
            Bytes datagram = PacketCodec::encode(rpt, payload);
            // 鎶ュ憡鐩村彂锛堜笉璧伴槦鍒楋級锛氫繚璇佹姇閫掔巼鏍锋湰鍙婃椂鍒拌揪鍙戦€佺锟?
            // btl 璺熻穼涓嶈鍙嶅悜绉帇鎷栨參锛堣 send_direct 娉ㄩ噴锛夛拷?
            send_direct(&peer, std::move(datagram));

            peer.m_last_report_sent = now;
        }
    }

    void check_offline() {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto now = std::chrono::steady_clock::now();
        for (auto& kv : m_peers) {
            auto& peer = kv.second;
            {
                std::lock_guard<std::mutex> lk(peer.m_mu);
                for (auto it = peer.m_incoming.begin(); it != peer.m_incoming.end();) {
                    if (now - it->second.m_first_seen >= m_config.dead_timeout) {
                        it = peer.m_incoming.erase(it);
                    } else {
                        ++it;
                    }
                }
                for (auto it = peer.m_completed.begin(); it != peer.m_completed.end();) {
                    if (now - it->second >= m_config.dead_timeout) {
                        it = peer.m_completed.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (peer.m_state != LinkState::Established && peer.m_state != LinkState::Online) continue;
            if (now - peer.m_last_recv >= m_config.dead_timeout) {
                peer.m_state = LinkState::Closed;
                {
                    // m_pending is also mutated by handle_ack/handle_report
                    // (receiver) and send_data_packet (encode) under peer.m_mu.
                    std::lock_guard<std::mutex> lk(peer.m_mu);
                    peer.m_pending.clear();
                    peer.m_control_pending.clear();
                }
                fire_peer_event(&peer, LinkState::Closed);
                if (peer.m_reconnect) {
                    peer.m_state = LinkState::Handshake;
                    peer.m_last_handshake_sent = now - m_config.retransmit_timeout;
                    peer.m_handshake_backoff = std::chrono::milliseconds(500);
                    peer.m_last_recv = now;
                }
            }
        }
    }

    void process_send_queue() {
        std::map<int, std::deque<SendMsg>> local;
        {
            std::lock_guard<std::mutex> lock(m_send_mutex);
            local.swap(m_send_queue);
        }
        for (auto it = local.rbegin(); it != local.rend(); ++it) {
            auto& queue = it->second;
            while (!queue.empty()) {
                auto item = std::move(queue.front());
                queue.pop_front();

                std::string peer_id = std::move(item.m_peer);
                Bytes payload = std::move(item.m_payload);
                std::uint8_t channel = item.m_channel;
                bool keyframe = item.m_keyframe;

                {
                    std::lock_guard<std::mutex> lock(m_peers_mutex);
                    auto pit = m_peers.find(peer_id);
                    if (pit == m_peers.end()) {
                        pit = std::find_if(m_peers.begin(), m_peers.end(), [&](const auto& entry) {
                            return entry.second.m_id == peer_id;
                        });
                    }
                    if (pit == m_peers.end()) continue;
                    auto& peer = pit->second;
                    // 通道排空：该通道消息出队即丢（三层检查之一，防止
                    // 排空期后旧消息从 send_queue 漏出继续编码/发送）
                    auto now_ms = unix_millis();
                    if (channel < 8 && now_ms < m_drain_until_ms[channel].load()) {
                        continue;
                    }
                    // NOTE: do not log here. When a peer is not Online the
                    // message is re-queued and retried every reactor tick, so
                    // a log line here fires thousands of times while holding
                    // m_peers_mutex -- starving the receiver thread (which
                    // also needs m_peers_mutex) and causing UDP receive-buffer
                    // loss.
                    // Established 鍗虫斁琛岋細鎻℃墜瀹屾垚鍚庝細璇濆瘑閽ュ弻鏂瑰氨缁紝
                    // 鏁版嵁娴佷笉搴斾緷璧栧崟锟?Online 閫氬憡鐨勫埌杈撅紙寮辩綉涓嬭
                    // 鎶ユ枃鍙兘涓㈠け涓旀棤閲嶄紶锛岃嫢浠ユ涓洪棬鎺т細閫犳垚瀵圭
                    // 姘镐箙鍗″湪 Established銆佸彂閫侀槦鍒楁案涓嶆帓绌猴級锟?
                    if (peer.m_state != LinkState::Online &&
                        peer.m_state != LinkState::Established) {
                        if (peer.m_state != LinkState::Closed) {
                            std::lock_guard<std::mutex> lk(m_send_mutex);
                            std::size_t total = 0;
                            for (const auto& kv : m_send_queue) total += kv.second.size();
                            total += m_encode_queue.size();
                            if (total < queue_limit()) {
                                // 绉诲姩璇箟鍥炲锛屾棤鎷疯礉
                                m_send_queue[it->first].emplace_back(
                                    SendMsg{peer_id, std::move(payload), channel, keyframe});
                            }
                        }
                        continue;
                    }

                    EncodeTask task{&peer, std::move(payload), channel, keyframe};
                    bool pushed = m_encode_queue.try_push(std::move(task));
                    if (!pushed) {
                        std::lock_guard<std::mutex> lk(m_send_mutex);
                        std::size_t total = 0;
                        for (const auto& kv : m_send_queue) total += kv.second.size();
                        total += m_encode_queue.size();
                        if (total < queue_limit()) {
                            m_send_queue[0].emplace_back(
                                SendMsg{peer_id, std::move(task.m_payload), task.m_channel,
                                        task.m_keyframe});
                        }
                    }
                }
            }
        }
    }

    void encode_loop() {
        while (m_running.load(std::memory_order_acquire) &&
               m_workers_running.load(std::memory_order_acquire)) {
            auto task = m_encode_queue.take_for(flush_interval());
            if (!task) continue;
            // 通道排空：该通道编码任务出队即丢（防止排空期后旧消息从
            // 管线漏出——send_queue 检查 + 此处检查 + sender 检查三层）
            auto now_ms = unix_millis();
            if (task->m_channel < 8 && now_ms < m_drain_until_ms[task->m_channel].load()) {
                continue;
            }
            try {
                fragment_and_send(task->m_peer, std::move(task->m_payload), task->m_channel, task->m_keyframe);
            } catch (const std::exception& e) {
                TIGHT_DBG_PRINTF("DBG encode-exc: %s\n", e.what());
            } catch (...) {
                TIGHT_DBG_PRINTF("DBG encode-exc: unknown\n");
            }
        }
        while (true) {
            auto task = m_encode_queue.poll();
            if (!task) break;
            try {
                fragment_and_send(task->m_peer, std::move(task->m_payload), task->m_channel, task->m_keyframe);
            } catch (...) {}
        }
    }

    void fragment_and_send(Peer* peer, Bytes payload, std::uint8_t channel,
                           bool keyframe = false) {
        // 两步台阶提升的 FEC 探测冗余（估计器 fec_probe_extra）：恢复提升
        // 第一步时压上 FEC 校验片负载感知链路（FEC 可丢失、不伤业务），
        // 第二步确认后移除、业务流量替换。仅作用于视频通道（0）。
        std::uint16_t probe_extra = m_bandwidth.fec_probe_extra();
        // 每通道固定 FEC 冗余（音频通道可单独加强）。探测冗余仅作用于
        // 视频通道（通道 0），避免探测流量干扰音频的稳定传输。
        std::uint16_t chan_extra = (channel < 8) ? m_config.channel_fec_extra[channel] : 0;
        if (channel != 0) probe_extra = 0;
        Fragmenter::fragment_and_send(*peer, std::move(payload), m_config.mtu,
                                      [this, channel](Peer* p, std::uint32_t msg_id,
                                             std::uint16_t idx, std::uint16_t cnt,
                                             std::uint16_t data_cnt, std::uint16_t real_size,
                                             const std::uint8_t* frag_data, std::size_t frag_len,
                                             std::size_t width, bool ackable, bool keyframe) {
                                          send_data_packet(p, msg_id, idx, cnt, data_cnt,
                                                           real_size, frag_data, frag_len,
                                                           width, ackable, channel, keyframe);
                                      },
                                      channel, chan_extra, probe_extra, keyframe);
    }

    void send_byes() {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        if (!socket_ready()) return;
        for (auto& kv : m_peers) {
            auto& peer = kv.second;
            if (peer.m_state == LinkState::Closed) continue;
            if (!peer.m_addr_set) continue;
            PacketHeader header{};
            header.magic = kMagic;
            header.version = kVersion;
            header.type = PacketType::Bye;
            header.client_id = m_local_client_id;
            header.session_id = m_local_session_id;
            Bytes datagram = PacketCodec::encode(header, Bytes{});
            socket_send(peer.m_addr, datagram.data(), datagram.size());
            peer.m_state = LinkState::Closed;
        }
    }
};

TightTransport::TightTransport(TightConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config))) {}

TightTransport::TightTransport(TightConfig config, std::shared_ptr<ISocket> socket)
    : m_impl(std::make_unique<Impl>(std::move(config), std::move(socket))) {}

TightTransport::~TightTransport() {
    if (m_impl) {
        m_impl->send_byes();
        m_impl->stop();
    }
}

void TightTransport::set_message_callback(MessageCallback callback) {
    m_impl->set_message_callback(std::move(callback));
}

void TightTransport::set_peer_callback(PeerCallback callback) {
    m_impl->set_peer_callback(std::move(callback));
}

void TightTransport::set_command_callback(CommandCallback callback) {
    m_impl->set_command_callback(std::move(callback));
}

bool TightTransport::start() {
    return m_impl->start();
}

void TightTransport::stop() {
    m_impl->send_byes();
    m_impl->stop();
}

void TightTransport::inject_packet(const NetAddress& from,
                                   const std::uint8_t* data, std::size_t size) {
    m_impl->inject_packet(from, data, size);
}

void TightTransport::inject_packet(const NetAddress& from, const Bytes& datagram) {
    m_impl->inject_packet(from, datagram.data(), datagram.size());
}

bool TightTransport::connect(const RemotePeer& remote) {
    return m_impl->connect(remote);
}

bool TightTransport::send(const std::string& peer_id, Bytes payload) {
    return m_impl->send_message(peer_id, std::move(payload), 0);
}

bool TightTransport::send_video(const std::string& peer_id, Bytes payload, bool keyframe) {
    return m_impl->send_message(peer_id, std::move(payload), 0, 0, keyframe);
}

bool TightTransport::send_channel(const std::string& peer_id, Bytes payload, std::uint8_t channel) {
    return m_impl->send_message(peer_id, std::move(payload), 0, channel);
}

bool TightTransport::send_priority(const std::string& peer_id, Bytes payload, int priority) {
    return m_impl->send_message(peer_id, std::move(payload), priority);
}

bool TightTransport::send_command(const std::string& peer_id, Bytes payload) {
    return m_impl->send_command(peer_id, std::move(payload));
}

void TightTransport::set_lite_mode(bool lite) {
    m_impl->set_lite_mode(lite);
}

bool TightTransport::lite_mode() const {
    return m_impl->m_lite_mode.load();
}

std::vector<PeerEvent> TightTransport::peers() const {
    return m_impl->peers_snapshot();
}

std::uint16_t TightTransport::local_port() const {
    return m_impl->m_local_port;
}

std::uint64_t TightTransport::estimated_bandwidth_bps() const {
    return m_impl->m_bandwidth.bytes_per_second();
}

std::uint64_t TightTransport::btl_bw_bps() const {
    return m_impl->m_bandwidth.btl_bw_bps();
}

std::uint64_t TightTransport::video_capacity_bps() const {
    return m_impl->video_capacity_bps();
}

double TightTransport::fec_redundancy_ratio() const {
    return m_impl->fec_redundancy_ratio();
}

void TightTransport::set_video_capacity_callback(VideoCapacityCallback callback) {
    std::lock_guard<std::mutex> lock(m_impl->m_callback_mutex);
    m_impl->m_video_capacity_cb = std::move(callback);
}

void TightTransport::set_loan_exhausted_callback(LoanExhaustedCallback callback) {
    std::lock_guard<std::mutex> lock(m_impl->m_callback_mutex);
    m_impl->m_loan_exhausted_cb = std::move(callback);
}

void TightTransport::set_evac_keyframe_callback(EvacKeyframeCallback callback) {
    std::lock_guard<std::mutex> lock(m_impl->m_callback_mutex);
    m_impl->m_evac_keyframe_cb = std::move(callback);
}

bool TightTransport::pacer_app_limited() const {
    return m_impl->m_bandwidth.app_limited_state();
}

bool TightTransport::pacer_limited() const {
    return m_impl->m_pacer_limited.load(std::memory_order_relaxed);
}

std::uint32_t TightTransport::peer_p50_ms(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(m_impl->m_peers_mutex);
    auto pit = m_impl->m_peers.find(peer_id);
    if (pit == m_impl->m_peers.end()) {
        pit = std::find_if(m_impl->m_peers.begin(), m_impl->m_peers.end(),
                           [&](const auto& entry) { return entry.second.m_id == peer_id; });
    }
    if (pit == m_impl->m_peers.end()) return 0;
    std::lock_guard<std::mutex> plk(pit->second.m_mu);
    return pit->second.m_peer_p50_ms;
}

std::size_t TightTransport::outbound_queue_size() const {
    std::size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_send_mutex);
        for (const auto& kv : m_impl->m_send_queue) total += kv.second.size();
    }
    total += m_impl->m_encode_queue.size();
    total += m_impl->m_outbound_queue.size();
    total += m_impl->m_audio_queue.size();
    return total;
}

void TightTransport::clear_outbound() {
    m_impl->clear_outbound_impl();
}

namespace {
// 默认排空时长：100ms（一个 flush 节拍内旧消息从 send→encode→outbound
// 管线完全退出 + 少量余量；QSV 编码器重启 ~300ms > 100ms，新 IDR 在
// 排空结束后生成，不会被误丢）
constexpr std::chrono::milliseconds kDefaultDrainMs{100};
}  // namespace

void TightTransport::drain_channel(std::uint8_t channel) {
    drain_channel(channel, kDefaultDrainMs);
}

void TightTransport::drain_channel(std::uint8_t channel, std::chrono::milliseconds duration) {
    if (channel >= 8) return;
    std::uint64_t until = unix_millis() + static_cast<std::uint64_t>(duration.count());
    m_impl->m_drain_until_ms[channel].store(until);
}

void TightTransport::set_message_loss_callback(MessageLossCallback callback) {
    m_impl->set_message_loss_callback(std::move(callback));
}

void TightTransport::set_file_callback(FileCallback callback) {
    m_impl->set_file_callback(std::move(callback));
}

void TightTransport::set_data_callback(DataCallback callback) {
    m_impl->set_data_callback(std::move(callback));
}

namespace {
// 澶х搴忓垪鍖栬緟鍔╋紙file/data 鍐呴儴娑堟伅鏍煎紡锟?
inline void put_be16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    std::uint16_t be = tight::tight_detail::to_be16(v);
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(&be);
    b.insert(b.end(), p, p + 2);
}
inline void put_be32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    std::uint32_t be = tight::tight_detail::to_be32(v);
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(&be);
    b.insert(b.end(), p, p + 4);
}
inline void put_be64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    std::uint64_t be = tight::tight_detail::to_be64(v);
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(&be);
    b.insert(b.end(), p, p + 8);
}
}

bool TightTransport::send_file(const std::string& peer_id, const std::string& name,
                               const Bytes& data) {
    if (name.size() > 65535) return false;
    std::uint32_t file_id = m_impl->m_file_id_out++;
    const std::uint32_t chunk_size = static_cast<std::uint32_t>(Impl::kFileChunkSize);
    const std::uint32_t chunk_count = data.empty()
        ? 1 : static_cast<std::uint32_t>((data.size() + chunk_size - 1) / chunk_size);
    // 娓呭崟锛歿0x01} file_id(4) name_len(2) name total(8) chunk_size(4) chunk_count(4)
    Bytes manifest;
    manifest.push_back(0x01);
    put_be32(manifest, file_id);
    put_be16(manifest, static_cast<std::uint16_t>(name.size()));
    manifest.insert(manifest.end(), name.begin(), name.end());
    put_be64(manifest, data.size());
    put_be32(manifest, chunk_size);
    put_be32(manifest, chunk_count);
    bool r1 = m_impl->send_message(peer_id, std::move(manifest), 0, Impl::kFileChannel);
    if (!r1) return false;
    for (std::uint32_t i = 0; i < chunk_count; ++i) {
        Bytes chunk;
        chunk.push_back(0x02);
        put_be32(chunk, file_id);
        put_be32(chunk, i);
        std::size_t off = static_cast<std::size_t>(i) * chunk_size;
        std::size_t len = std::min<std::size_t>(chunk_size, data.size() - off);
        chunk.insert(chunk.end(), data.begin() + off, data.begin() + off + len);
        bool r = m_impl->send_message(peer_id, std::move(chunk), 0, Impl::kFileChannel);
        if (!r) return false;
    }
    return true;
}

bool TightTransport::send_data(const std::string& peer_id, Bytes payload) {
    Bytes msg;
    msg.push_back(0x03);
    msg.insert(msg.end(), payload.begin(), payload.end());
    return m_impl->send_message(peer_id, std::move(msg), 0, Impl::kDataChannel);
}

std::uint64_t TightTransport::file_data_pending_bytes() const {
    std::uint64_t total = 0;
    std::lock_guard<std::mutex> lock(m_impl->m_send_mutex);
    for (const auto& kv : m_impl->m_send_queue) {
        for (const auto& m : kv.second) {
            if (m.m_channel == Impl::kFileChannel || m.m_channel == Impl::kDataChannel) {
                total += m.m_payload.size();
            }
        }
    }
    return total;
}

}
