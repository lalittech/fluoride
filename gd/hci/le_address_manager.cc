#include "hci/le_address_manager.h"
#include "os/log.h"
#include "os/rand.h"

namespace bluetooth {
namespace hci {

static constexpr uint8_t BLE_ADDR_MASK = 0xc0u;

LeAddressManager::LeAddressManager(
    common::Callback<void(std::unique_ptr<CommandPacketBuilder>)> enqueue_command,
    os::Handler* handler,
    Address public_address,
    uint8_t connect_list_size,
    uint8_t resolving_list_size)
    : enqueue_command_(enqueue_command),
      handler_(handler),
      public_address_(public_address),
      connect_list_size_(connect_list_size),
      resolving_list_size_(resolving_list_size){};

LeAddressManager::~LeAddressManager() {
  if (address_rotation_alarm_ != nullptr) {
    address_rotation_alarm_->Cancel();
    address_rotation_alarm_.reset();
  }
}

// Aborts if called more than once
void LeAddressManager::SetPrivacyPolicyForInitiatorAddress(
    AddressPolicy address_policy,
    AddressWithType fixed_address,
    crypto_toolbox::Octet16 rotation_irk,
    std::chrono::milliseconds minimum_rotation_time,
    std::chrono::milliseconds maximum_rotation_time) {
  ASSERT(address_policy_ == AddressPolicy::POLICY_NOT_SET);
  ASSERT(address_policy != AddressPolicy::POLICY_NOT_SET);
  ASSERT_LOG(registered_clients_.empty(), "Policy must be set before clients are registered.");
  address_policy_ = address_policy;

  switch (address_policy_) {
    case AddressPolicy::USE_PUBLIC_ADDRESS:
      le_address_ = fixed_address;
      break;
    case AddressPolicy::USE_STATIC_ADDRESS: {
      auto addr = fixed_address.GetAddress();
      auto address = addr.address;
      // The two most significant bits of the static address shall be equal to 1
      ASSERT_LOG((address[5] & BLE_ADDR_MASK) == BLE_ADDR_MASK, "The two most significant bits shall be equal to 1");
      // Bits of the random part of the address shall not be all 1 or all 0
      if ((address[0] == 0x00 && address[1] == 0x00 && address[2] == 0x00 && address[3] == 0x00 && address[4] == 0x00 &&
           address[5] == BLE_ADDR_MASK) ||
          (address[0] == 0xFF && address[1] == 0xFF && address[2] == 0xFF && address[3] == 0xFF && address[4] == 0xFF &&
           address[5] == 0xFF)) {
        LOG_ALWAYS_FATAL("Bits of the random part of the address shall not be all 1 or all 0");
      }
      le_address_ = fixed_address;
      auto packet = hci::LeSetRandomAddressBuilder::Create(le_address_.GetAddress());
      handler_->Post(common::BindOnce(enqueue_command_, std::move(packet)));
    } break;
    case AddressPolicy::USE_NON_RESOLVABLE_ADDRESS:
    case AddressPolicy::USE_RESOLVABLE_ADDRESS:
      rotation_irk_ = rotation_irk;
      minimum_rotation_time_ = minimum_rotation_time;
      maximum_rotation_time_ = maximum_rotation_time;
      address_rotation_alarm_ = std::make_unique<os::Alarm>(handler_);
      break;
    case AddressPolicy::POLICY_NOT_SET:
      LOG_ALWAYS_FATAL("invalid parameters");
  }
}

// TODO(jpawlowski): remove once we have config file abstraction in cert tests
void LeAddressManager::SetPrivacyPolicyForInitiatorAddressForTest(
    AddressPolicy address_policy,
    AddressWithType fixed_address,
    crypto_toolbox::Octet16 rotation_irk,
    std::chrono::milliseconds minimum_rotation_time,
    std::chrono::milliseconds maximum_rotation_time) {
  ASSERT(address_policy != AddressPolicy::POLICY_NOT_SET);
  ASSERT_LOG(registered_clients_.empty(), "Policy must be set before clients are registered.");
  address_policy_ = address_policy;

  switch (address_policy_) {
    case AddressPolicy::USE_PUBLIC_ADDRESS:
      le_address_ = fixed_address;
      break;
    case AddressPolicy::USE_STATIC_ADDRESS: {
      auto addr = fixed_address.GetAddress();
      auto address = addr.address;
      // The two most significant bits of the static address shall be equal to 1
      ASSERT_LOG((address[5] & BLE_ADDR_MASK) == BLE_ADDR_MASK, "The two most significant bits shall be equal to 1");
      // Bits of the random part of the address shall not be all 1 or all 0
      if ((address[0] == 0x00 && address[1] == 0x00 && address[2] == 0x00 && address[3] == 0x00 && address[4] == 0x00 &&
           address[5] == BLE_ADDR_MASK) ||
          (address[0] == 0xFF && address[1] == 0xFF && address[2] == 0xFF && address[3] == 0xFF && address[4] == 0xFF &&
           address[5] == 0xFF)) {
        LOG_ALWAYS_FATAL("Bits of the random part of the address shall not be all 1 or all 0");
      }
      le_address_ = fixed_address;
      auto packet = hci::LeSetRandomAddressBuilder::Create(le_address_.GetAddress());
      handler_->Call(enqueue_command_, std::move(packet));
    } break;
    case AddressPolicy::USE_NON_RESOLVABLE_ADDRESS:
    case AddressPolicy::USE_RESOLVABLE_ADDRESS:
      rotation_irk_ = rotation_irk;
      minimum_rotation_time_ = minimum_rotation_time;
      maximum_rotation_time_ = maximum_rotation_time;
      address_rotation_alarm_ = std::make_unique<os::Alarm>(handler_);
      break;
    case AddressPolicy::POLICY_NOT_SET:
      LOG_ALWAYS_FATAL("invalid parameters");
  }
}

LeAddressManager::AddressPolicy LeAddressManager::Register(LeAddressManagerCallback* callback) {
  handler_->BindOnceOn(this, &LeAddressManager::register_client, callback).Invoke();
  return address_policy_;
}

void LeAddressManager::register_client(LeAddressManagerCallback* callback) {
  registered_clients_.insert(std::pair<LeAddressManagerCallback*, ClientState>(callback, ClientState::RESUMED));
  if (address_policy_ == AddressPolicy::POLICY_NOT_SET || address_policy_ == AddressPolicy::USE_RESOLVABLE_ADDRESS ||
      address_policy_ == AddressPolicy::USE_NON_RESOLVABLE_ADDRESS) {
    prepare_to_rotate();
  }
}

void LeAddressManager::Unregister(LeAddressManagerCallback* callback) {
  handler_->BindOnceOn(this, &LeAddressManager::unregister_client, callback).Invoke();
}

void LeAddressManager::unregister_client(LeAddressManagerCallback* callback) {
  registered_clients_.erase(callback);
  if (registered_clients_.empty() && address_rotation_alarm_ != nullptr) {
    address_rotation_alarm_->Cancel();
  }
}

void LeAddressManager::AckPause(LeAddressManagerCallback* callback) {
  handler_->BindOnceOn(this, &LeAddressManager::ack_pause, callback).Invoke();
}

void LeAddressManager::AckResume(LeAddressManagerCallback* callback) {
  handler_->BindOnceOn(this, &LeAddressManager::ack_resume, callback).Invoke();
}

AddressWithType LeAddressManager::GetCurrentAddress() {
  ASSERT(address_policy_ != AddressPolicy::POLICY_NOT_SET);
  return le_address_;
}

AddressWithType LeAddressManager::GetAnotherAddress() {
  ASSERT(
      address_policy_ == AddressPolicy::USE_NON_RESOLVABLE_ADDRESS ||
      address_policy_ == AddressPolicy::USE_RESOLVABLE_ADDRESS);
  hci::Address address = generate_rpa();
  auto random_address = AddressWithType(address, AddressType::RANDOM_DEVICE_ADDRESS);
  return random_address;
}

void LeAddressManager::pause_registered_clients() {
  for (auto client : registered_clients_) {
    if (client.second != ClientState::PAUSED && client.second != ClientState::WAITING_FOR_PAUSE) {
      client.second = ClientState::WAITING_FOR_PAUSE;
      client.first->OnPause();
    }
  }
}

void LeAddressManager::ack_pause(LeAddressManagerCallback* callback) {
  ASSERT(registered_clients_.find(callback) != registered_clients_.end());
  registered_clients_.find(callback)->second = ClientState::PAUSED;
  for (auto client : registered_clients_) {
    if (client.second != ClientState::PAUSED) {
      // make sure all client paused
      return;
    }
  }
  handle_next_command();
}

void LeAddressManager::resume_registered_clients() {
  // Do not resume clients if cached command is not empty
  if (!cached_commands_.empty()) {
    handle_next_command();
    return;
  }

  for (auto client : registered_clients_) {
    client.second = ClientState::WAITING_FOR_RESUME;
    client.first->OnResume();
  }
}

void LeAddressManager::ack_resume(LeAddressManagerCallback* callback) {
  ASSERT(registered_clients_.find(callback) != registered_clients_.end());
  registered_clients_.find(callback)->second = ClientState::RESUMED;
}

void LeAddressManager::prepare_to_rotate() {
  Command command = {CommandType::ROTATE_RANDOM_ADDRESS, nullptr};
  cached_commands_.push(std::move(command));
  pause_registered_clients();
}

void LeAddressManager::rotate_random_address() {
  if (address_policy_ != AddressPolicy::USE_RESOLVABLE_ADDRESS &&
      address_policy_ != AddressPolicy::USE_NON_RESOLVABLE_ADDRESS) {
    return;
  }

  address_rotation_alarm_->Schedule(
      common::BindOnce(&LeAddressManager::prepare_to_rotate, common::Unretained(this)),
      get_next_private_address_interval_ms());

  hci::Address address;
  if (address_policy_ == AddressPolicy::USE_RESOLVABLE_ADDRESS) {
    address = generate_rpa();
  } else {
    address = generate_nrpa();
  }
  auto packet = hci::LeSetRandomAddressBuilder::Create(address);
  enqueue_command_.Run(std::move(packet));
  le_address_ = AddressWithType(address, AddressType::RANDOM_DEVICE_ADDRESS);
}

void LeAddressManager::on_le_set_random_address_complete(CommandCompleteView view) {
  auto complete_view = LeSetRandomAddressCompleteView::Create(view);
  if (!complete_view.IsValid()) {
    LOG_ALWAYS_FATAL("Received on_le_set_random_address_complete with invalid packet");
  } else if (complete_view.GetStatus() != ErrorCode::SUCCESS) {
    auto status = complete_view.GetStatus();
    std::string error_code = ErrorCodeText(status);
    LOG_ALWAYS_FATAL("Received on_le_set_random_address_complete with error code %s", error_code.c_str());
  }
  if (cached_commands_.empty()) {
    handler_->BindOnceOn(this, &LeAddressManager::resume_registered_clients).Invoke();
  } else {
    handler_->BindOnceOn(this, &LeAddressManager::handle_next_command).Invoke();
  }
}

/* This function generates Resolvable Private Address (RPA) from Identity
 * Resolving Key |irk| and |prand|*/
hci::Address LeAddressManager::generate_rpa() {
  // most significant bit, bit7, bit6 is 01 to be resolvable random
  // Bits of the random part of prand shall not be all 1 or all 0
  std::array<uint8_t, 3> prand = os::GenerateRandom<3>();
  constexpr uint8_t BLE_RESOLVE_ADDR_MSB = 0x40;
  prand[2] &= ~BLE_ADDR_MASK;
  if ((prand[0] == 0x00 && prand[1] == 0x00 && prand[2] == 0x00) ||
      (prand[0] == 0xFF && prand[1] == 0xFF && prand[2] == 0x3F)) {
    prand[0] = (uint8_t)(os::GenerateRandom() % 0xFE + 1);
  }
  prand[2] |= BLE_RESOLVE_ADDR_MSB;

  hci::Address address;
  address.address[3] = prand[0];
  address.address[4] = prand[1];
  address.address[5] = prand[2];

  /* encrypt with IRK */
  crypto_toolbox::Octet16 p = crypto_toolbox::aes_128(rotation_irk_, prand.data(), 3);

  /* set hash to be LSB of rpAddress */
  address.address[0] = p[0];
  address.address[1] = p[1];
  address.address[2] = p[2];
  return address;
}

// This function generates NON-Resolvable Private Address (NRPA)
hci::Address LeAddressManager::generate_nrpa() {
  // The two most significant bits of the address shall be equal to 0
  // Bits of the random part of the address shall not be all 1 or all 0
  std::array<uint8_t, 6> random = os::GenerateRandom<6>();
  random[5] &= ~BLE_ADDR_MASK;
  if ((random[0] == 0x00 && random[1] == 0x00 && random[2] == 0x00 && random[3] == 0x00 && random[4] == 0x00 &&
       random[5] == 0x00) ||
      (random[0] == 0xFF && random[1] == 0xFF && random[2] == 0xFF && random[3] == 0xFF && random[4] == 0xFF &&
       random[5] == 0x3F)) {
    random[0] = (uint8_t)(os::GenerateRandom() % 0xFE + 1);
  }

  hci::Address address;
  address.FromOctets(random.data());

  // the address shall not be equal to the public address
  while (address == public_address_) {
    address.address[0] = (uint8_t)(os::GenerateRandom() % 0xFE + 1);
  }

  return address;
}

std::chrono::milliseconds LeAddressManager::get_next_private_address_interval_ms() {
  auto interval_random_part_max_ms = maximum_rotation_time_ - minimum_rotation_time_;
  auto random_ms = std::chrono::milliseconds(os::GenerateRandom()) % (interval_random_part_max_ms);
  return minimum_rotation_time_ + random_ms;
}

uint8_t LeAddressManager::GetConnectListSize() {
  return connect_list_size_;
}

uint8_t LeAddressManager::GetResolvingListSize() {
  return resolving_list_size_;
}

void LeAddressManager::handle_next_command() {
  ASSERT(!cached_commands_.empty());
  auto command = std::move(cached_commands_.front());
  cached_commands_.pop();

  if (command.command_type == CommandType::ROTATE_RANDOM_ADDRESS) {
    rotate_random_address();
  } else {
    enqueue_command_.Run(std::move(command.command_packet));
  }
}

void LeAddressManager::AddDeviceToConnectList(
    ConnectListAddressType connect_list_address_type, bluetooth::hci::Address address) {
  auto packet_builder = hci::LeAddDeviceToConnectListBuilder::Create(connect_list_address_type, address);
  Command command = {CommandType::ADD_DEVICE_TO_CONNECT_LIST, std::move(packet_builder)};
  handler_->BindOnceOn(this, &LeAddressManager::pause_registered_clients).Invoke();
  cached_commands_.push(std::move(command));
}

void LeAddressManager::AddDeviceToResolvingList(
    PeerAddressType peer_identity_address_type,
    Address peer_identity_address,
    const std::array<uint8_t, 16>& peer_irk,
    const std::array<uint8_t, 16>& local_irk) {
  auto packet_builder = hci::LeAddDeviceToResolvingListBuilder::Create(
      peer_identity_address_type, peer_identity_address, peer_irk, local_irk);
  Command command = {CommandType::ADD_DEVICE_TO_RESOLVING_LIST, std::move(packet_builder)};
  handler_->BindOnceOn(this, &LeAddressManager::pause_registered_clients).Invoke();
  cached_commands_.push(std::move(command));
}

void LeAddressManager::RemoveDeviceFromConnectList(
    ConnectListAddressType connect_list_address_type, bluetooth::hci::Address address) {
  auto packet_builder = hci::LeRemoveDeviceFromConnectListBuilder::Create(connect_list_address_type, address);
  Command command = {CommandType::REMOVE_DEVICE_FROM_CONNECT_LIST, std::move(packet_builder)};
  handler_->BindOnceOn(this, &LeAddressManager::pause_registered_clients).Invoke();
  cached_commands_.push(std::move(command));
}

void LeAddressManager::RemoveDeviceFromResolvingList(
    PeerAddressType peer_identity_address_type, Address peer_identity_address) {
  auto packet_builder =
      hci::LeRemoveDeviceFromResolvingListBuilder::Create(peer_identity_address_type, peer_identity_address);
  Command command = {CommandType::REMOVE_DEVICE_FROM_RESOLVING_LIST, std::move(packet_builder)};
  handler_->BindOnceOn(this, &LeAddressManager::pause_registered_clients).Invoke();
  cached_commands_.push(std::move(command));
}

void LeAddressManager::ClearConnectList() {
  auto packet_builder = hci::LeClearConnectListBuilder::Create();
  Command command = {CommandType::CLEAR_CONNECT_LIST, std::move(packet_builder)};
  handler_->BindOnceOn(this, &LeAddressManager::pause_registered_clients).Invoke();
  cached_commands_.push(std::move(command));
}

void LeAddressManager::ClearResolvingList() {
  auto packet_builder = hci::LeClearResolvingListBuilder::Create();
  Command command = {CommandType::CLEAR_RESOLVING_LIST, std::move(packet_builder)};
  handler_->BindOnceOn(this, &LeAddressManager::pause_registered_clients).Invoke();
  cached_commands_.push(std::move(command));
}

void LeAddressManager::OnCommandComplete(bluetooth::hci::CommandCompleteView view) {
  if (!view.IsValid()) {
    LOG_ERROR("Received command complete with invalid packet");
    return;
  }
  std::string op_code = OpCodeText(view.GetCommandOpCode());
  LOG_DEBUG("Received command complete with op_code %s", op_code.c_str());

  // The command was sent before any client registered, we can make sure all the clients paused when command complete.
  if (view.GetCommandOpCode() == OpCode::LE_SET_RANDOM_ADDRESS &&
      address_policy_ == AddressPolicy::USE_STATIC_ADDRESS) {
    LOG_DEBUG("Received LE_SET_RANDOM_ADDRESS complete and Address policy is USE_STATIC_ADDRESS, return");
    return;
  }

  if (cached_commands_.empty()) {
    handler_->BindOnceOn(this, &LeAddressManager::resume_registered_clients).Invoke();
  } else {
    handler_->BindOnceOn(this, &LeAddressManager::handle_next_command).Invoke();
  }
}

}  // namespace hci
}  // namespace bluetooth
