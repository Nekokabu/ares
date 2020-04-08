CartridgeSlot cartridgeSlot{"Cartridge Slot"};

CartridgeSlot::CartridgeSlot(string_view name) : name(name) {
}

auto CartridgeSlot::load(Node::Object parent, Node::Object from) -> void {
  port = Node::append<Node::Port>(parent, from, name);
  port->setFamily("PC Engine");
  port->setType("Cartridge");
  port->setAllocate([&] { return Node::Peripheral::create(interface->name()); });
  port->setAttach([&](auto node) { connect(node); });
  port->setDetach([&](auto node) { disconnect(); });
  port->scan(from);
}

auto CartridgeSlot::unload() -> void {
  disconnect();
  port = {};
}

auto CartridgeSlot::connect(Node::Peripheral node) -> void {
  disconnect();
  if(node) {
    cartridge.connect(port, node);
  }
}

auto CartridgeSlot::disconnect() -> void {
  if(connected()) {
    cartridge.disconnect();
  }
}
