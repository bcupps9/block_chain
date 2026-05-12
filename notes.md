To implement a distributed timestamp server on a peer-to-peer basis, we will need to use a proof-
of-work system similar to Adam Back's Hashcash [6], rather than newspaper or Usenet posts.
The proof-of-work involves scanning for a value that when hashed, such as with SHA-256, the
hash begins with a number of zero bits. The average work required is exponential in the number
of zero bits required and can be verified by executing a single hash.


You as a peer server request the block. what if a fake server sends it to you? IP spoofing here once a node has one block


what is a merkle tree?

Ownership = "I can produce a signature satisfying this UTXO's locking script."


Plan:
Some fraction of full nodes are miners
User's will only exists on full nodes for simplicity

Coroutines that are run:

Submit Transaction
Receive message
Verify Block
Update UTXO
Gossip Block
Gossip Transaction
Ammend Chain (from either new chain proposed or new thing asked for)
(Miners only)
Assemble block
    - Mine 

Normal course run:
UTXO is our blockchain version of transaction log
User transaction
    - build transaction using UTXO's, sign each input with private key
    - broadcasts to a full node
    - full node inserts in mempool and gossips
Full node verification and gossip mempool
Miner block creation with transaction
Miners add a block to the chain and gossip it.
User gives itself a confirmation every time a new block is built on top of the block that it cares about (stop after 6 or so)



Notes:
There is no such thing as a view change or "recovery of a node" that is necessary, all the nodes have incentives that align to get them back on the program to increase the amount of work backing the integrity of our transaction log

What will I have to simplify:
The work? 
The transaction wrapping?
The incentive structure? All simulated nodes work at the same speed. I can give them their code then say that the incentives match rather than have the incentives create the code (or where time is spent)
Dealing with private and public keys (make everything public at first? )


Eclipse attack (one thing that breaks)

Overwhelming CPU usage (A coordinated attacker through multiple )



eddie doesn't think that implementing a simulator

THe best form for this project is not something that involves cotamer

Cotamer is just message loss and stuff. this is not the failure that we are trying to model in the bitcoin example.

mostly analytically we can see that paxos fails if half or more of the failures fail. 

you can cause double spend if you have this 

Read some more generally

Then do the reading of the failure stuff

Then, 3rd part is set up a real model that gets close to these analytcial bounds that i have come up with