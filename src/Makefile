CC      = c++
CPP     = c++
#PROF    = -pg
DEBUG   = -g
WARN    = -Wall
OPT     = -O2
# -m32: the scripting code stores pointers in int, so TFE must be built
# 32-bit. -std, -fno-strict-aliasing and -fno-delete-null-pointer-checks
# keep current GCC from optimising on rules this 2000s-era code breaks
# (it type-puns pointers and null-checks inside its own strcasecmp).
STD     = -std=gnu++17 -fno-strict-aliasing -fno-delete-null-pointer-checks
CFLAGS  = $(DEBUG) ${OPT} $(WARN) $(PROF) $(STD) -m32
C_FLAGS = $(CFLAGS) 
# The SSH listener (ssh.cc) uses libssh; it needs the 32-bit libssh-devel.
SSH_LIBS = -lssh
VPATH   = ./OBJ

DIR = ./OBJ

C_FILES = abil.cc account.cc acid.cc act.cc action.cc affect.cc alias.cc \
          align.cc appear.cc area.cc armor.cc auction.cc ban.cc bank.cc \
          bard.cc bash.cc bit.cc bow.cc bug.cc cards.cc channel.cc \
          char.cc char_name.cc clan.cc  \
          cleric.cc code.cc code2.cc cold.cc color.cc corpse.cc  \
          custom.cc damage.cc death.cc descr.cc dictionary.cc display.cc drink.cc \
          druid.cc edit.cc enemy.cc exit.cc event.cc event2.cc  \
          exp.cc familiar.cc fight.cc fight2.cc  \
          fight3.cc file.cc find.cc fire.cc flag.cc flee.cc  \
          food.cc get.cc give.cc group.cc \
          handler.cc help.cc hide.cc host.cc html.cc \
          imm.cc interp.cc kick.cc leech.cc level.cc library.cc \
          lib_exit.cc lib_fight.cc lib_logic.cc lib_list.cc \
	  lib_object.cc lib_path.cc lib_quest.cc lib_time.cc lib_vehicle.cc \
          light.cc list.cc log.cc login.cc look.cc \
          macros.cc mage.cc magic.cc magic2.cc \
          memory.cc misc.cc mob_imm.cc  \
          mobile.cc money.cc  \
          monk.cc mount.cc move.cc move2.cc \
          mprog.cc network.cc newbie.cc \
          note.cc number.cc obj_imm.cc obj_name.cc obj_prop.cc \
          obj_sort.cc object.cc object2.cc \
          oprog.cc option.cc output.cc paladin.cc path.cc player.cc \
          pet.cc pos.cc  \
          program.cc prompt.cc psi.cc put.cc quest.cc  \
          ranger.cc recipe.cc recog.cc regen.cc relig.cc \
          reput.cc rescue.cc  \
	  reset.cc room.cc rtable.cc save.cc score.cc \
          search.cc set.cc shock.cc \
          shop.cc skill.cc social.cc sound.cc \
          spell.cc ssh.cc startup.cc string.cc table.cc teleport.cc \
          tfe.cc thief.cc thing.cc \
          throw.cc time.cc tprog.cc track.cc trainer.cc undead.cc \
          update.cc variable.cc vehicle.cc verb.cc vote.cc \
	  wand.cc water.cc wear.cc \
          weather.cc weight.cc where.cc who.cc

O_FILES = $(C_FILES:.cc=.o)


all: $(O_FILES)
	rm -f tfe
	$(CC) $(C_FLAGS) -o tfe $(DIR)/*.o $(L_FLAGS) $(SSH_LIBS)
	rm -f daemon         
	$(CC) $(C_FLAGS) -o daemon daemon.cc $(L_FLAGS)

clean:
	rm -f OBJ/*.o
	rm -f core core.* tfe daemon
	rm -f email.msg
	rm -f Makefile.bak
	rm -f *~

depend: $(C_FILES)
	makedepend -- $(CFLAGS) -- $(C_FILES)

.cc.o:
	@mkdir -p $(DIR)
	$(CPP) -c $(C_FLAGS) -I. $< -o $(DIR)/$@

install: all
	cp tfe ../bin/
	cp daemon ../bin/

